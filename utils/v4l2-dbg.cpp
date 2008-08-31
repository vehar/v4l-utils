/*
    Copyright (C) 2007  Hans Verkuil <hverkuil@xs4all.nl>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <unistd.h>
#include <features.h>		/* Uses _GNU_SOURCE to define getsubopt in stdlib.h */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <ctype.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <math.h>
#include <sys/klog.h>

#include <linux/videodev2.h>
#include <linux/i2c-id.h>
#include <media/v4l2-chip-ident.h>

#include <list>
#include <vector>
#include <map>
#include <string>

#include "v4l2-dbg-bttv.h"
#include "v4l2-dbg-saa7134.h"
#include "v4l2-dbg-em28xx.h"

#define ARRAY_SIZE(arr) ((int)(sizeof(arr) / sizeof((arr)[0])))

struct board_list {
	const char *name;
	int prefix; 		/* Register prefix size */
	const struct board_regs *regs;
	int regs_size;
	const struct board_regs *alt_regs;
	int alt_regs_size;
};

static const struct board_list boards[] = {
	{				/* From bttv-dbg.h */
		BTTV_IDENT,
		sizeof(BTTV_PREFIX) - 1,
		bt8xx_regs,
		ARRAY_SIZE(bt8xx_regs),
		bt8xx_regs_other,
		ARRAY_SIZE(bt8xx_regs_other),
	},
	{				/* From saa7134-dbg.h */
		SAA7134_IDENT,
		sizeof(SAA7134_PREFIX) - 1,
		saa7134_regs,
		ARRAY_SIZE(saa7134_regs),
		NULL,
		0,
	},
	{				/* From em28xx-dbg.h */
		EM28XX_IDENT,
		sizeof(EM28XX_PREFIX) - 1,
		em28xx_regs,
		ARRAY_SIZE(em28xx_regs),
		NULL,
		0,
	},
};

struct driverid {
	const char *name;
	unsigned id;
};

extern struct driverid driverids[];

struct chipid {
	const char *name;
	unsigned id;
};

extern struct chipid chipids[];

/* Short option list

   Please keep in alphabetical order.
   That makes it easier to see which short options are still free.

   In general the lower case is used to set something and the upper
   case is used to retrieve a setting. */
enum Option {
	OptListRegisters = 'l',
	OptGetRegister = 'g',
	OptSetRegister = 's',
	OptSetDevice = 'd',
	OptGetDriverInfo = 'D',
	OptChip = 'c',
	OptScanChipIdents = 'S',
	OptGetChipIdent = 'i',
	OptSetStride = 'w',
	OptHelp = 'h',

	OptLogStatus = 128,
	OptVerbose,
	OptListDriverIDs,
	OptListSymbols,
	OptLast = 256
};

static char options[OptLast];

static unsigned capabilities;

static struct option long_options[] = {
	{"device", required_argument, 0, OptSetDevice},
	{"help", no_argument, 0, OptHelp},
	{"list-registers", optional_argument, 0, OptListRegisters},
	{"get-register", required_argument, 0, OptGetRegister},
	{"set-register", required_argument, 0, OptSetRegister},
	{"chip", required_argument, 0, OptChip},
	{"scan-chip-idents", no_argument, 0, OptScanChipIdents},
	{"get-chip-ident", required_argument, 0, OptGetChipIdent},
	{"info", no_argument, 0, OptGetDriverInfo},
	{"verbose", no_argument, 0, OptVerbose},
	{"log-status", no_argument, 0, OptLogStatus},
	{"list-driverids", no_argument, 0, OptListDriverIDs},
	{"list-symbols", no_argument, 0, OptListSymbols},
	{"wide", required_argument, 0, OptSetStride},
	{0, 0, 0, 0}
};

static void usage(void)
{
	printf("Usage: v4l2-dbg [options] [values]\n"
	       "  -D, --info         Show driver info [VIDIOC_QUERYCAP]\n"
	       "  -d, --device=<dev> Use device <dev> instead of /dev/video0\n"
	       "                     If <dev> is a single digit, then /dev/video<dev> is used\n"
	       "  -h, --help         Display this help message\n"
	       "  --verbose          Turn on verbose ioctl error reporting\n"
	       "  -c, --chip=<chip>  The chip identifier to use with other commands\n"
	       "                     It can be one of:\n"
	       "                         I2C driver ID (see --list-driverids)\n"
	       "                         I2C 7-bit address\n"
	       "                         host<num>: host chip number <num>\n"
	       "                         host (default): same as host0\n"
	       "  -l, --list-registers[=min=<addr>[,max=<addr>]]\n"
	       "		     Dump registers from <min> to <max> [VIDIOC_DBG_G_REGISTER]\n"
	       "  -g, --get-register=<addr>\n"
	       "		     Get the specified register [VIDIOC_DBG_G_REGISTER]\n"
	       "  -s, --set-register=<addr>\n"
	       "		     Set the register with the commandline arguments\n"
	       "                     The register will autoincrement [VIDIOC_DBG_S_REGISTER]\n"
	       "  -S, --scan-chip-idents\n"
	       "		     Scan the available host and i2c chips [VIDIOC_G_CHIP_IDENT]\n"
	       "  -i, --get-chip-ident\n"
	       "		     Get the chip identifier [VIDIOC_G_CHIP_IDENT]\n"
	       "  -w, --wide=<reg length>\n"
	       "		     Sets step between two registers\n"
	       "  --list-symbols     List the symbolic register names you can use, if any\n"
	       "  --log-status       Log the board status in the kernel log [VIDIOC_LOG_STATUS]\n"
	       "  --list-driverids   List the known I2C driver IDs for use with the i2cdrv type\n");
	exit(0);
}

static unsigned parse_chip(const std::string &s)
{
	for (int i = 0; driverids[i].name; i++)
		if (!strcasecmp(s.c_str(), driverids[i].name))
			return driverids[i].id;
	return 0;
}

static std::string cap2s(unsigned cap)
{
	std::string s;

	if (cap & V4L2_CAP_VIDEO_CAPTURE)
		s += "\t\tVideo Capture\n";
	if (cap & V4L2_CAP_VIDEO_OUTPUT)
		s += "\t\tVideo Output\n";
	if (cap & V4L2_CAP_VIDEO_OVERLAY)
		s += "\t\tVideo Overlay\n";
	if (cap & V4L2_CAP_VIDEO_OUTPUT_OVERLAY)
		s += "\t\tVideo Output Overlay\n";
	if (cap & V4L2_CAP_VBI_CAPTURE)
		s += "\t\tVBI Capture\n";
	if (cap & V4L2_CAP_VBI_OUTPUT)
		s += "\t\tVBI Output\n";
	if (cap & V4L2_CAP_SLICED_VBI_CAPTURE)
		s += "\t\tSliced VBI Capture\n";
	if (cap & V4L2_CAP_SLICED_VBI_OUTPUT)
		s += "\t\tSliced VBI Output\n";
	if (cap & V4L2_CAP_RDS_CAPTURE)
		s += "\t\tRDS Capture\n";
	if (cap & V4L2_CAP_TUNER)
		s += "\t\tTuner\n";
	if (cap & V4L2_CAP_AUDIO)
		s += "\t\tAudio\n";
	if (cap & V4L2_CAP_RADIO)
		s += "\t\tRadio\n";
	if (cap & V4L2_CAP_READWRITE)
		s += "\t\tRead/Write\n";
	if (cap & V4L2_CAP_ASYNCIO)
		s += "\t\tAsync I/O\n";
	if (cap & V4L2_CAP_STREAMING)
		s += "\t\tStreaming\n";
	return s;
}

static void print_regs(int fd, struct v4l2_register *reg, unsigned long min, unsigned long max, int stride)
{
	unsigned long mask = stride > 1 ? 0x1f : 0x0f;
	unsigned long i;
	int line = 0;

	for (i = min & ~mask; i <= max; i += stride) {
		if ((i & mask) == 0 && line % 32 == 0) {
			if (stride == 4)
				printf("\n                00       04       08       0C       10       14       18       1C");
			else
				printf("\n          00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F");
		}

		if ((i & mask) == 0) {
			printf("\n%08lx: ", i);
			line++;
		}
		if (i < min) {
			printf("%*s ", 2 * stride, "");
			continue;
		}
		reg->reg = i;
		if (ioctl(fd, VIDIOC_DBG_G_REGISTER, reg) < 0) {
			fprintf(stderr, "ioctl: VIDIOC_DBG_G_REGISTER "
					"failed for 0x%llx\n", reg->reg);
		} else {
			printf("%0*llx ", 2 * stride, reg->val);
		}
		usleep(1);
	}
	printf("\n");
}

static void print_chip(struct v4l2_chip_ident *chip)
{
	const char *name = NULL;

	for (int i = 0; chipids[i].name; i++) {
		if (chipids[i].id == chip->ident) {
			name = chipids[i].name;
			break;
		}
	}
	if (name)
		printf("%-10s revision 0x%08x\n", name, chip->revision);
	else
		printf("%-10d revision 0x%08x\n", chip->ident, chip->revision);
}

static unsigned long long parse_reg(const struct board_list *curr_bd, const std::string &reg)
{
	if (curr_bd) {
		for (int i = 0; i < curr_bd->regs_size; i++) {
			if (!strcasecmp(reg.c_str(), curr_bd->regs[i].name) ||
			    !strcasecmp(reg.c_str(), curr_bd->regs[i].name + curr_bd->prefix)) {
				return curr_bd->regs[i].reg;
			}
		}
		for (int i = 0; i < curr_bd->alt_regs_size; i++) {
			if (!strcasecmp(reg.c_str(), curr_bd->alt_regs[i].name) ||
			    !strcasecmp(reg.c_str(), curr_bd->alt_regs[i].name + curr_bd->prefix)) {
				return curr_bd->alt_regs[i].reg;
			}
		}
	}
	return strtoull(reg.c_str(), NULL, 0);
}

static const char *binary(unsigned long long val)
{
	static char bin[80];
	char *p = bin;
	int i, j;
	int bits = 64;

	if ((val & 0xffffffff00000000LL) == 0) {
		if ((val & 0xffff0000) == 0) {
			if ((val & 0xff00) == 0)
				bits = 8;
			else
				bits= 16;
		}
		else
			bits = 32;
	}

	for (i = bits - 1; i >= 0; i -= 8) {
		for (j = i; j >= i - 7; j--) {
			if (val & (1LL << j))
				*p++ = '1';
			else
				*p++ = '0';
		}
		*p++ = ' ';
	}
	p[-1] = 0;
	return bin;
}

static int doioctl(int fd, int request, void *parm, const char *name)
{
	int retVal;

	if (!options[OptVerbose]) return ioctl(fd, request, parm);
	retVal = ioctl(fd, request, parm);
	printf("%s: ", name);
	if (retVal < 0)
		printf("failed: %s\n", strerror(errno));
	else
		printf("ok\n");

	return retVal;
}

static int parse_subopt(char **subs, const char * const *subopts, char **value)
{
	int opt = getsubopt(subs, (char * const *)subopts, value);

	if (opt == -1) {
		fprintf(stderr, "Invalid suboptions specified\n");
		usage();
		exit(1);
	}
	if (value == NULL) {
		fprintf(stderr, "No value given to suboption <%s>\n",
				subopts[opt]);
		usage();
		exit(1);
	}
	return opt;
}

int main(int argc, char **argv)
{
	char *value, *subs;
	int i, forcedstride = 0;

	int fd = -1;

	/* command args */
	int ch;
	const char *device = "/dev/video0";	/* -d device */
	struct v4l2_capability vcap;	/* list_cap */
	struct v4l2_register set_reg;
	struct v4l2_register get_reg;
	struct v4l2_chip_ident chip_id;
	const struct board_list *curr_bd = NULL;
	char short_options[26 * 2 * 2 + 1];
	int idx = 0;
	std::string reg_min_arg, reg_max_arg;
	std::string reg_set_arg;
	unsigned long long reg_min = 0, reg_max = 0;
	std::vector<std::string> get_regs;
	int match_type = V4L2_CHIP_MATCH_HOST;
	int match_chip = 0;

	memset(&set_reg, 0, sizeof(set_reg));
	memset(&get_reg, 0, sizeof(get_reg));
	memset(&chip_id, 0, sizeof(chip_id));

	if (argc == 1) {
		usage();
		return 0;
	}
	for (i = 0; long_options[i].name; i++) {
		if (!isalpha(long_options[i].val))
			continue;
		short_options[idx++] = long_options[i].val;
		if (long_options[i].has_arg == required_argument)
			short_options[idx++] = ':';
	}
	while (1) {
		int option_index = 0;

		short_options[idx] = 0;
		ch = getopt_long(argc, argv, short_options,
				 long_options, &option_index);
		if (ch == -1)
			break;

		options[(int)ch] = 1;
		switch (ch) {
		case OptHelp:
			usage();
			return 0;

		case OptSetDevice:
			device = optarg;
			if (device[0] >= '0' && device[0] <= '9' && device[1] == 0) {
				static char newdev[20];
				char dev = device[0];

				sprintf(newdev, "/dev/video%c", dev);
				device = newdev;
			}
			break;

		case OptChip:
			if (isdigit(optarg[0])) {
				match_type = V4L2_CHIP_MATCH_I2C_ADDR;
				match_chip = strtoul(optarg, NULL, 0);
				break;
			}
			if (!memcmp(optarg, "host", 4)) {
				match_type = V4L2_CHIP_MATCH_HOST;
				match_chip = strtoul(optarg + 4, NULL, 0);
				break;
			}
			match_type = V4L2_CHIP_MATCH_I2C_DRIVER;
			match_chip = parse_chip(optarg);
			if (!match_chip) {
				fprintf(stderr, "unknown driver ID %s\n", optarg);
				exit(-1);
			}
			break;

		case OptSetRegister:
			reg_set_arg = optarg;
			break;

		case OptGetRegister:
			get_regs.push_back(optarg);
			break;

		case OptSetStride:
			forcedstride = strtoull(optarg, 0L, 0);
			break;

		case OptListRegisters:
			subs = optarg;
			if (subs == NULL)
				break;
			while (*subs != '\0') {
				static const char * const subopts[] = {
					"min",
					"max",
					NULL
				};

				switch (parse_subopt(&subs, subopts, &value)) {
				case 0:
					reg_min_arg = value;
					//if (reg_max == 0)
					//	reg_max = reg_min + 0xff;
					break;
				case 1:
					reg_max_arg = value;
					break;
				}
			}
			break;

		case OptGetChipIdent:
		case OptListSymbols:
			break;

		case ':':
			fprintf(stderr, "Option `%s' requires a value\n",
				argv[optind]);
			usage();
			return 1;

		case '?':
			fprintf(stderr, "Unknown argument `%s'\n",
				argv[optind]);
			usage();
			return 1;
		}
	}

	if ((fd = open(device, O_RDWR)) < 0) {
		fprintf(stderr, "Failed to open %s: %s\n", device,
			strerror(errno));
		exit(1);
	}

	doioctl(fd, VIDIOC_QUERYCAP, &vcap, "VIDIOC_QUERYCAP");
	capabilities = vcap.capabilities;

	/* Information Opts */

	if (options[OptGetDriverInfo]) {
		printf("Driver info:\n");
		printf("\tDriver name   : %s\n", vcap.driver);
		printf("\tCard type     : %s\n", vcap.card);
		printf("\tBus info      : %s\n", vcap.bus_info);
		printf("\tDriver version: %d\n", vcap.version);
		printf("\tCapabilities  : 0x%08X\n", vcap.capabilities);
		printf("%s", cap2s(vcap.capabilities).c_str());
	}

	for (int board = ARRAY_SIZE(boards) - 1; board >= 0; board--) {
		if (!strcasecmp((char *)vcap.driver, boards[board].name)) {
			curr_bd = &boards[board];
			break;
		}
	}

	/* Set options */

	if (options[OptSetRegister]) {
		set_reg.match_type = match_type;
		set_reg.match_chip = match_chip;
		if (optind >= argc)
			usage();
		set_reg.reg = parse_reg(curr_bd, reg_set_arg);
		while (optind < argc) {
			set_reg.val = strtoull(argv[optind++], NULL, 0);
			if (doioctl(fd, VIDIOC_DBG_S_REGISTER, &set_reg,
						"VIDIOC_DBG_S_REGISTER") == 0)
				printf("register 0x%llx set to 0x%llx\n", set_reg.reg, set_reg.val);
			set_reg.reg++;
		}
	}

	if (options[OptGetChipIdent]) {
		chip_id.match_type = match_type;
		chip_id.match_chip = match_chip;
		if (doioctl(fd, VIDIOC_G_CHIP_IDENT, &chip_id, "VIDIOC_G_CHIP_IDENT") == 0)
			print_chip(&chip_id);
	}

	if (options[OptScanChipIdents]) {
		int i;

		chip_id.match_type = V4L2_CHIP_MATCH_HOST;
		chip_id.match_chip = 0;

		while (doioctl(fd, VIDIOC_G_CHIP_IDENT, &chip_id, "VIDIOC_G_CHIP_IDENT") == 0 && chip_id.ident) {
			printf("host%d: ", chip_id.match_chip);
			print_chip(&chip_id);
			chip_id.match_chip++;
		}

		chip_id.match_type = V4L2_CHIP_MATCH_I2C_ADDR;
		for (i = 0; i < 128; i++) {
			chip_id.match_chip = i;
			if (doioctl(fd, VIDIOC_G_CHIP_IDENT, &chip_id, "VIDIOC_G_CHIP_IDENT") == 0 && chip_id.ident) {
				printf("i2c 0x%02x: ", i);
				print_chip(&chip_id);
			}
		}
	}

	if (options[OptGetRegister]) {
		int stride = 1;

		get_reg.match_type = match_type;
		get_reg.match_chip = match_chip;
		printf("ioctl: VIDIOC_DBG_G_REGISTER\n");

		for (std::vector<std::string>::iterator iter = get_regs.begin();
				iter != get_regs.end(); ++iter) {
			get_reg.reg = parse_reg(curr_bd, *iter);
			if (ioctl(fd, VIDIOC_DBG_G_REGISTER, &get_reg) < 0)
				fprintf(stderr, "ioctl: VIDIOC_DBG_G_REGISTER "
						"failed for 0x%llx\n", get_reg.reg);
			else
				printf("%llx = %llxh = %lldd = %sb\n", get_reg.reg,
					get_reg.val, get_reg.val, binary(get_reg.val));
		}
	}

	if (options[OptListRegisters]) {
		int stride = 1;

		get_reg.match_type = match_type;
		get_reg.match_chip = match_chip;
		if (forcedstride) {
			stride = forcedstride;
		} else {
			if (get_reg.match_type == V4L2_CHIP_MATCH_HOST)
				stride = 4;
		}
		printf("ioctl: VIDIOC_DBG_G_REGISTER\n");

		if (!reg_min_arg.empty()) {
			reg_min = parse_reg(curr_bd, reg_min_arg);
			if (reg_max_arg.empty())
				reg_max = reg_min + 0xff;
			else
				reg_max = parse_reg(curr_bd, reg_max_arg);
			/* Explicit memory range: just do it */
			print_regs(fd, &get_reg, reg_min, reg_max, stride);
			goto list_done;
		}
		/* try to match the i2c chip */
		switch (get_reg.match_chip) {
		case I2C_DRIVERID_SAA711X:
			print_regs(fd, &get_reg, 0, 0xff, stride);
			break;
		case I2C_DRIVERID_SAA717X:
			// FIXME: use correct reg regions
			print_regs(fd, &get_reg, 0, 0xff, stride);
			break;
		case I2C_DRIVERID_SAA7127:
			print_regs(fd, &get_reg, 0, 0x7f, stride);
			break;
		case I2C_DRIVERID_CX25840:
			print_regs(fd, &get_reg, 0, 2, stride);
			print_regs(fd, &get_reg, 0x100, 0x15f, stride);
			print_regs(fd, &get_reg, 0x200, 0x23f, stride);
			print_regs(fd, &get_reg, 0x400, 0x4bf, stride);
			print_regs(fd, &get_reg, 0x800, 0x9af, stride);
			break;
		case I2C_DRIVERID_CS5345:
			print_regs(fd, &get_reg, 1, 0x10, stride);
			break;
		case 0:
			/* host chip, handle later */
			break;
		default:
			/* unknown i2c chip, dump 0-0xff by default */
			print_regs(fd, &get_reg, 0, 0xff, stride);
			break;
		}
		if (get_reg.match_chip != 0) {
			/* found i2c chip, we're done */
			goto list_done;
		}
		/* try to figure out which host chip it is */
		if (doioctl(fd, VIDIOC_G_CHIP_IDENT, &chip_id, "VIDIOC_G_CHIP_IDENT") != 0) {
			chip_id.ident = V4L2_IDENT_NONE;
		}

		switch (chip_id.ident) {
		case V4L2_IDENT_CX23415:
		case V4L2_IDENT_CX23416:
			print_regs(fd, &get_reg, 0x02000000, 0x020000ff, stride);
			break;
		case V4L2_IDENT_CX23418:
			print_regs(fd, &get_reg, 0x02c40000, 0x02c409c7, stride);
			break;
		default:
			/* By default print range 0-0xff */
			print_regs(fd, &get_reg, 0, 0xff, stride);
			break;
		}
	}
list_done:

	if (options[OptLogStatus]) {
		static char buf[40960];
		int len;

		if (doioctl(fd, VIDIOC_LOG_STATUS, NULL, "VIDIOC_LOG_STATUS") == 0) {
			printf("\nStatus Log:\n\n");
			len = klogctl(3, buf, sizeof(buf) - 1);
			if (len >= 0) {
				char *p = buf;
				char *q;

				buf[len] = 0;
				while ((q = strstr(p, "START STATUS CARD #"))) {
					p = q + 1;
				}
				if (p) {
					while (p > buf && *p != '<') p--;
					q = p;
					while ((q = strstr(q, "<6>"))) {
						memcpy(q, "   ", 3);
					}
					printf("%s", p);
				}
			}
		}
	}

	if (options[OptListDriverIDs]) {
		printf("Known I2C driver IDs:\n");
		for (int i = 0; driverids[i].name; i++)
			printf("%s\n", driverids[i].name);
	}

	if (options[OptListSymbols]) {
		if (curr_bd == NULL) {
			printf("No symbols found for driver %s\n", vcap.driver);
		}
		else {
			printf("Symbols for driver %s:\n", vcap.driver);
			for (int i = 0; i < curr_bd->regs_size; i++)
				printf("0x%08x: %s\n", curr_bd->regs[i], curr_bd->regs[i].name);
			for (int i = 0; i < curr_bd->alt_regs_size; i++)
				printf("0x%08x: %s\n", curr_bd->alt_regs[i], curr_bd->alt_regs[i].name);
		}
	}

	close(fd);
	exit(0);
}
