/*
 * Copyright (C) 2017, Boundary Devices <info@boundarydevices.com>
 *
 * SPDX-License-Identifier:      GPL-2.0+
 *
 * Tool to read/write FT5x06 touch controller firmware
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

/* Documented registers */
#define ID_G_THGROUP		0x80
#define ID_G_THPEAK		0x81
#define ID_G_THCAL		0x82
#define ID_G_THWATER		0x83
#define ID_G_THTEMP		0x84
#define ID_G_CTRL		0x86
#define ID_G_TIME_ENTER_MONITOR	0x87
#define ID_G_PERIODACTIVE	0x88
#define ID_G_PERIODMONITOR	0x89
#define ID_G_AUTO_CLB_MODE	0xa0
#define ID_G_LIB_VERSION_H	0xa1
#define ID_G_LIB_VERSION_L	0xa2
#define ID_G_CIPHER		0xa3
#define ID_G_MODE		0xa4
#define ID_G_FIRMID		0xa6
#define ID_G_FT5201ID		0xa8
#define ID_G_ERR		0xa9
#define ID_G_CLB		0xaa
#define ID_G_B_AREA_TH		0xae
#define FT5x06_MAX_REG_OFFSET	0xae

/* Undocumented registers */
#define FT_FW_READ_REG		0x03
#define FT_REG_RESET_FW		0x07
#define FT_ERASE_APP_REG	0x61
#define FT_ERASE_PANEL_REG	0x63
#define FT_FLASH_STATUS		0x6a
#define FT_PARAM_READ_REG	0x85
#define FT_READ_ID_REG		0x90
#define FT_FW_START_REG		0xbf
#define FT_REG_ECC		0xcc
#define FT_RST_CMD_REG1		0xfc

/* Undocumented firmware update values */
#define FT_UPGRADE_AA		0xAA
#define FT_UPGRADE_55		0x55
#define FT_UPGRADE_LOOP		30
#define FT_FW_MIN_SIZE		8
#define FT_FW_MAX_SIZE		64*1024
#define FT_FW_NAME_MAX_LEN	50
#define FT_MAX_TRIES		5
#define FT_RETRY_DLY		20
#define FT_MAX_WR_BUF		10
#define FT_MAX_RD_BUF		2
#define FT_FW_PKT_LEN		128
#define FT_FW_PKT_READ_LEN	256
#define FT_FW_PKT_META_LEN	6
#define FT_FW_PKT_DLY_MS	20

/* Chip ID that we consider correct */
#define FT5x06_ID	0x55
#define FT5x16_ID	0x0a
#define FT5x26_ID	0x54

/* Print macros */
#define LOG(fmt, arg...) fprintf(stdout, "[%s]: " fmt "\n" , __func__ , ## arg)
#define ERR(fmt, arg...) fprintf(stderr, "[%s]: " fmt "\n" , __func__ , ## arg)
#ifndef DEBUG
#define DBG(fmt, arg...) {}
#else
#define DBG(fmt, arg...) fprintf(stdout, "[%s]: " fmt "\n" , __func__ , ## arg)
#endif

static inline void msleep(int delay) { usleep(delay*1000); }

struct ft5x06_ts {
	int fd;
	uint8_t addr;
	uint8_t chip_id;
	uint8_t	fw_ver;
};

struct ft5x06_fw_update_info {
	uint8_t chip_id;
	char fts_name[20];
	uint8_t tpd_max_points;
	uint8_t auto_clb;
	uint16_t delay_aa;		/*delay of write FT_UPGRADE_AA */
	uint16_t delay_55;		/*delay of write FT_UPGRADE_55 */
	uint8_t upgrade_id_1;		/*upgrade id 1 */
	uint8_t upgrade_id_2;		/*upgrade id 2 */
	uint16_t delay_readid;		/*delay of read id */
	uint16_t delay_erase_flash;	/*delay of erase flash*/
	uint32_t flash_offset;
};

/*
 * Update information taken from FocalTech (bloated) driver:
 * https://github.com/focaltech-systems/drivers-input-touchscreen-FTS_driver
 */
struct ft5x06_fw_update_info ft5x06_fwu_info[] = {
	{FT5x06_ID, "ft5x06", 5, 1, 50,  30, 0x79, 0x03, 10, 2000, 0x0000},
	{FT5x16_ID, "ft5x16", 5, 1, 50,  30, 0x79, 0x07, 10, 1500, 0x0000},
	{FT5x26_ID, "ft5x26", 5, 0,  4, 250, 0x54, 0x2c, 10, 3000, 0x1800},
};

static int ft5x06_i2c_read(int fd, int addr, uint8_t *wrbuf, uint16_t wrlen,
			   uint8_t *rdbuf, uint16_t rdlen)
{
	struct i2c_rdwr_ioctl_data data;
	int ret;

	if (wrlen > 0) {
		struct i2c_msg msgs[] = {
			{ addr, 0, wrlen, wrbuf },
			{ addr, I2C_M_RD, rdlen, rdbuf },
		};
		data.msgs  = msgs;
		data.nmsgs = ARRAY_SIZE(msgs);
		ret = ioctl(fd, I2C_RDWR, &data);
	} else {
		struct i2c_msg msgs[] = {
			{ addr, I2C_M_RD, rdlen, rdbuf },
		};
		data.msgs  = msgs;
		data.nmsgs = ARRAY_SIZE(msgs);
		ret = ioctl(fd, I2C_RDWR, &data);
	}

	if (ret < 0)
		ERR("Error %d", ret);

	return ret;
}

static int ft5x06_i2c_write(int fd, int addr, uint8_t *buf, uint16_t len)
{
	int ret;
	struct i2c_rdwr_ioctl_data data;
	struct i2c_msg msgs[] = {
		{ addr, 0, len, buf },
	};

	data.msgs  = msgs;
	data.nmsgs = ARRAY_SIZE(msgs);

	ret = ioctl(fd, I2C_RDWR, &data);
	if (ret < 0)
		ERR("Error %d", ret);

	return ret;
}

static void ft5x06_write_reg(int fd, int addr, uint8_t regnum, uint8_t value)
{
	uint8_t regnval[] = {
		regnum,
		value
	};

	ft5x06_i2c_write(fd, addr, regnval, ARRAY_SIZE(regnval));
}

static char *ft5x06_get_name(unsigned chip_id)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(ft5x06_fwu_info); i++)
		if (chip_id == ft5x06_fwu_info[i].chip_id)
			return ft5x06_fwu_info[i].fts_name;

	return NULL;
}

static struct ft5x06_fw_update_info *ft5x06_get_info(unsigned chip_id)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(ft5x06_fwu_info); i++)
		if (chip_id == ft5x06_fwu_info[i].chip_id)
			return &ft5x06_fwu_info[i];

	return NULL;
}

/* Undocumented function but necessary for ft5426 */
static void ft5x26_hid_to_i2c(int fd, int addr)
{
	uint8_t packet_buf[3] = { 0xeb, 0xaa, 0x09 };

	ft5x06_i2c_write(fd, addr, packet_buf, 3);

	memset(packet_buf, 0, ARRAY_SIZE(packet_buf));

	ft5x06_i2c_read(fd, addr, packet_buf, 0, packet_buf, 3);
	if ((0xeb != packet_buf[0]) || (0xaa != packet_buf[1]) ||
	    (0x08 != packet_buf[2]))
		DBG("Failed %x %x %x", packet_buf[0],
		    packet_buf[1], packet_buf[2]);

	msleep(10);
}

static int ft5x06_read_id(int fd, int addr, int chip_id)
{
	struct ft5x06_fw_update_info *info = ft5x06_get_info(chip_id);
	uint8_t reg_val[2] = {0};
	uint8_t packet_buf[4];

	msleep(info->delay_readid);
	packet_buf[0] = FT_READ_ID_REG;
	packet_buf[1] = packet_buf[2] = packet_buf[3] = 0x00;

	ft5x06_i2c_read(fd, addr, packet_buf, 4, reg_val, 2);
	if (reg_val[0] != info->upgrade_id_1
	    || reg_val[1] != info->upgrade_id_2) {
		ERR("READ-ID not ok: %x %x", reg_val[0], reg_val[1]);
		return -1;
	}

	return 0;
}

static void ft5x06_reset_ctpm(int fd, int addr, int chip_id)
{
	struct ft5x06_fw_update_info *info = ft5x06_get_info(chip_id);

	ft5x06_write_reg(fd, addr, FT_RST_CMD_REG1, FT_UPGRADE_AA);
	msleep(info->delay_aa);
	ft5x06_write_reg(fd, addr, FT_RST_CMD_REG1, FT_UPGRADE_55);
	msleep(info->delay_55);
}

static void ft5x06_reset_fw(int fd, int addr)
{
	uint8_t packet_buf = FT_REG_RESET_FW;

	ft5x06_i2c_write(fd, addr, &packet_buf, 1);

	msleep(100);
}

static int ft5x06_init_upgrade(int fd, int addr, int chip_id)
{
	int i, ret;
	uint8_t packet_buf[4];

	for (i = 0; i < FT_UPGRADE_LOOP; i++) {
		/* Step 1: Reset CTPM */
		LOG("Reset CTPM");
		ft5x06_reset_ctpm(fd, addr, chip_id);

		/* Step 2: Enter upgrade mode */
		LOG("Enter upgrade mode");
		if (chip_id == FT5x26_ID)
			ft5x26_hid_to_i2c(fd, addr);
		packet_buf[0] = FT_UPGRADE_55;
		packet_buf[1] = FT_UPGRADE_AA;
		ret = ft5x06_i2c_write(fd, addr, packet_buf, 2);
		if (ret < 0) {
			ERR("failed to enter upgrade mode (%d)", ret);
			continue;
		}

		/* Step 3: Check READ-ID */
		LOG("Check READ-ID");
		if (ft5x06_read_id(fd, addr, chip_id) == 0)
			break;
	}

	if (i >= FT_UPGRADE_LOOP)
		return -EIO;

	return 0;
}

static void ft5x06_fw_send_packet(int fd, int addr, uint8_t command,
				  uint32_t offset, uint32_t length,
				  const uint8_t *data, uint8_t *ecc)
{
	uint8_t packet_buf[FT_FW_PKT_LEN + 6];
	int i;

	LOG("Write pkt [%x] @%x - len %d", command, offset, length);
	packet_buf[0] = command;
	packet_buf[1] = 0x00;
	packet_buf[2] = (uint8_t) (offset >> 8);
	packet_buf[3] = (uint8_t) offset;
	packet_buf[4] = (uint8_t) (length >> 8);
	packet_buf[5] = (uint8_t) length;
	for (i = 0; i < length; i++) {
		packet_buf[6 + i] = data[offset + i];
		*ecc ^= packet_buf[6 + i];
	}

	ft5x06_i2c_write(fd, addr, packet_buf, length + 6);
#if 0 /* Used for non FT5426 */
	msleep(FT_FW_PKT_DLY_MS);
#else
	for (i = 0; i < 5; i++) {
		uint8_t reg_val[2] = {0};
		uint32_t pkt_num = offset / FT_FW_PKT_LEN;

		msleep(5);
		packet_buf[0] = FT_FLASH_STATUS;
		ft5x06_i2c_read(fd, addr, packet_buf, 1, reg_val, 2);
		if ((pkt_num + 0x1000) == (((reg_val[0]) << 8) | reg_val[1]))
			break;
	}
#endif
}

static int ft5x06_fw_receive_packet(int fd, int addr, uint8_t command,
				    uint32_t offset, uint32_t length,
				    uint8_t *data)
{
	uint8_t packet_buf[4];

	LOG("Read pkt [%x] @%x - len %d", command, offset, length);
	packet_buf[0] = command;
	packet_buf[1] = 0x00;
	packet_buf[2] = (uint8_t) (offset >> 8);
	packet_buf[3] = (uint8_t) offset;

	return ft5x06_i2c_read(fd, addr, packet_buf, ARRAY_SIZE(packet_buf),
			       data, length);
}

static int ft5x06_fw_read(int fd, int addr, int chip_id, int outfd)
{
	int i, ret;
	uint32_t size = FT_FW_MAX_SIZE;
	uint8_t packet_buf[4];
	uint8_t data[FT_FW_PKT_READ_LEN];

	ret = ft5x06_init_upgrade(fd, addr, chip_id);
	if (ret < 0)
		return ret;

	LOG("Read the FW from flash");
	for (i = 0; i < size; i += FT_FW_PKT_READ_LEN) {
		uint32_t length = FT_FW_PKT_READ_LEN;

		if ((size - i) < FT_FW_PKT_READ_LEN)
			length = (size - i);

		msleep(10);
		ret = ft5x06_fw_receive_packet(fd, addr, FT_FW_READ_REG, i,
					       length, data);
		if (ret < 0)
			return ret;
		write(outfd, data, FT_FW_PKT_READ_LEN);
	}

	LOG("Reset the new FW");
	packet_buf[0] = FT_REG_RESET_FW;
	ft5x06_i2c_write(fd, addr, packet_buf, 1);
	msleep(100);

	return 0;
}

static int ft5x06_fw_upgrade(int fd, int addr, int chip_id,
			     const uint8_t *data, uint32_t data_len)
{
	struct ft5x06_fw_update_info *info = ft5x06_get_info(chip_id);
	int i, ret;
	uint8_t reg_val[4] = {0};
	uint8_t packet_buf[6];
	uint8_t ecc = 0;

	if (info == NULL)
		return -ENODEV;

	ret = ft5x06_init_upgrade(fd, addr, chip_id);
	if (ret < 0)
		return ret;

	LOG("Erase current app");
	packet_buf[0] = FT_ERASE_APP_REG;
	ft5x06_i2c_write(fd, addr, packet_buf, 1);
	if (chip_id != FT5x26_ID) {
		packet_buf[0] = FT_ERASE_PANEL_REG;
		ft5x06_i2c_write(fd, addr, packet_buf, 1);
	}
	msleep(info->delay_erase_flash);

	/* Prepare the system to receive a new firmware? */
	packet_buf[0] = 0xB0;
	packet_buf[1] = (uint8_t)((data_len >> 16) & 0xFF);
	packet_buf[2] = (uint8_t)((data_len >> 8) & 0xFF);
	packet_buf[3] = (uint8_t)(data_len & 0xFF);
	ft5x06_i2c_write(fd, addr, packet_buf, 4);

	LOG("Write firmware to CTPM flash");
	for (i = 0; i < data_len; i += FT_FW_PKT_LEN) {
		uint32_t length = FT_FW_PKT_LEN;

		if ((data_len - i) < FT_FW_PKT_LEN)
			length = (data_len - i);

		ft5x06_fw_send_packet(fd, addr, FT_FW_START_REG, i,
				      length, data, &ecc);
	}

	msleep(50);

	LOG("Verify checksum");
#if 0 /* FT5426 checksum method doesn't seem to work */
	packet_buf[0] = 0x64;
	ft5x06_i2c_write(fd, addr, packet_buf, 1);
	msleep(50);

	memset(packet_buf, 0, ARRAY_SIZE(packet_buf));
	packet_buf[0] = 0x65;
	packet_buf[4] = (uint8_t)((data_len) >> 8);
	packet_buf[5] = (uint8_t)(data_len);
	ret = ft5x06_i2c_write(fd, addr, packet_buf, 6);
	msleep(data_len / 256);

	/* Check flash status */
	for (i = 0; i < 100; i++) {
		packet_buf[0] = FT_FLASH_STATUS;
		ft5x06_i2c_read(fd, addr, packet_buf, 1, reg_val, 2);
		if (0xF0 == reg_val[0] && 0x55 == reg_val[1])
			break;
		msleep(1);
	}
	packet_buf[0] = 0x66;
#else
	packet_buf[0] = FT_REG_ECC;
#endif
	ft5x06_i2c_read(fd, addr, packet_buf, 1, reg_val, 1);
	if (reg_val[0] != ecc) {
		ERR("ECC error %02x vs. %02x", reg_val[0], ecc);
		return -EIO;
	}

	LOG("Reset the new FW");
	ft5x06_reset_fw(fd, addr);

	return 0;
}

static void show_help(const char *name)
{
	printf
	    ("FT5x06 tool usage: %s [OPTIONS]\nOPTIONS:\n"
	     "\t-a, --address\n\t\tI2C address of the FT5x06 controller (hex). "
	     "Default is 0x38.\n"
	     "\t-b, --bus\n\t\tI2C bus the FT5x06 controller is on. "
	     "Default is 2.\n"
	     "\t-c, --chipid\n\t\tForce chip ID to the value (hex). "
	     "Default is read from controller.\n"
	     "\t-i, --input\n\t\tInput firmware file to flash.\n"
	     "\t-o, --output\n\t\tOutput firmware file read from FT5x06.\n"
	     "\t-h, --help\n\t\tShow this help and exit.\n", name);
	return;
}

int main(int argc, const char *argv[])
{
	const char *input = NULL, *output = NULL;
	char dev[16];
	uint8_t *buffer;
	uint8_t wbuf, rbuf;
	int fd, ret;
	int arg_count = 1;
	int bus = 2;
	int addr = 0x38;
	int chip_id = -1;

	/* Parse all parameters */
	while (arg_count < argc) {
		if ((strcmp(argv[arg_count], "-a") == 0)
		    || (strcmp(argv[arg_count], "--address") == 0)) {
			addr = strtol(argv[++arg_count], NULL, 16);
		} else if ((strcmp(argv[arg_count], "-b") == 0)
			   || (strcmp(argv[arg_count], "--bus") == 0)) {
			bus = strtol(argv[++arg_count], NULL, 10);
		} else if ((strcmp(argv[arg_count], "-c") == 0)
			   || (strcmp(argv[arg_count], "--chipid") == 0)) {
			chip_id = strtol(argv[++arg_count], NULL, 16);
		} else if ((strcmp(argv[arg_count], "-i") == 0)
			   || (strcmp(argv[arg_count], "--input") == 0)) {
			input = argv[++arg_count];
		} else if ((strcmp(argv[arg_count], "-o") == 0)
			   || (strcmp(argv[arg_count], "--ouput") == 0)) {
			output = argv[++arg_count];
		} else {
			show_help(argv[0]);
			exit(1);
		}
		arg_count++;
	}

	sprintf(dev, "/dev/i2c-%d", bus);
	LOG("Opening %s", dev);
	fd = open(dev, O_RDWR);
	if (fd < 0) {
		LOG("Couldn't open %s: %s", dev, strerror(errno));
		return fd;
	}

	LOG("Setting addr to %#02x", addr);
	ret = ioctl(fd, I2C_SLAVE_FORCE, addr);
	if (ret != 0) {
		LOG("Couldn't set slave addr: %s", strerror(errno));
		return -1;
	}

	/* If chip ID isn't forced, detect it */
	if (chip_id < 0) {
		wbuf = ID_G_CIPHER;
		ret = ft5x06_i2c_read(fd, addr, &wbuf, 1, &rbuf, 1);
		if (ret < 0) {
			ERR("Couldn't get ID (%d)", ret);
			goto end;
		}
		chip_id = rbuf;
	}
	if (!ft5x06_get_name(chip_id)) {
		ERR("Unsupported chip ID: %x", chip_id);
		goto end;
	}
	LOG("Chip ID: %#x (%s)", chip_id, ft5x06_get_name(chip_id));

	/* Get current firmware version */
	wbuf = ID_G_FIRMID;
	ret = ft5x06_i2c_read(fd, addr, &wbuf, 1, &rbuf, 1);
	if (ret < 0) {
		ERR("Couldn't get ID (%d)", ret);
		goto end;
	}
	LOG("Firmware version: %d.0.0", rbuf);

	if (!input && !output) {
		LOG("Nothing to do (read or write)");
		goto end;
	}

	/* First read the firmware if asked for */
	if (output != NULL) {
		int outfd = open(output, O_RDWR | O_CREAT);
		if (outfd < 0) {
			ERR("Unable to open file %s", output);
			goto end;
		}
		ret = ft5x06_fw_read(fd, addr, chip_id, outfd);
		if (ret < 0)
			ERR("Failed to read FW");
		close(outfd);
	}

	/* Then flash a new firmware if available */
	if (input != NULL) {
		struct stat sb;
		int infd = open(input, O_RDONLY);
		if (infd < 0) {
			ERR("Unable to open file %s", input);
			goto end;
		}

		LOG("Flashing %s", input);
		fstat(infd, &sb);
		LOG("FW length is %ld", sb.st_size);

		buffer = mmap(NULL, sb.st_size, PROT_READ, MAP_SHARED, infd, 0);
		if (buffer == MAP_FAILED) {
			ERR("Couldn't map: %s", strerror(errno));
			close(infd);
			goto end;
		}
		ret = ft5x06_fw_upgrade(fd, addr, chip_id, buffer, sb.st_size);
		if (ret < 0)
			ERR("Failed to flash FW");
		close(infd);
		munmap(buffer, sb.st_size);
	}
end:
	close(fd);
	return 0;
}
