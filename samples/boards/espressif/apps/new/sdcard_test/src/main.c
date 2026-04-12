/*
 * SD Card Test — Phase 6: Module 11
 * Initialize SDMMC, mount FAT, write a file, read it back, and list the root directory.
 */

#include <string.h>
#include <zephyr/device.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/disk_access.h>

#include <ff.h>

LOG_MODULE_REGISTER(sdcard_test, LOG_LEVEL_INF);

#define DISK_DRIVE_NAME "SD"
#define DISK_MOUNT_PT "/SD:"
#define TEST_FILE_PATH DISK_MOUNT_PT "/TEST.TXT"

static FATFS fat_fs;

static struct fs_mount_t mount_info = {
	.type = FS_FATFS,
	.fs_data = &fat_fs,
	.mnt_point = DISK_MOUNT_PT,
};

static int list_dir(const char *path)
{
	struct fs_dir_t dir;
	struct fs_dirent entry;
	int ret;

	fs_dir_t_init(&dir);
	ret = fs_opendir(&dir, path);
	if (ret != 0) {
		LOG_ERR("fs_opendir(%s) failed: %d", path, ret);
		return ret;
	}

	printk("\nListing %s\n", path);
	while (true) {
		ret = fs_readdir(&dir, &entry);
		if (ret != 0) {
			LOG_ERR("fs_readdir failed: %d", ret);
			break;
		}

		if (entry.name[0] == '\0') {
			ret = 0;
			break;
		}

		if (entry.type == FS_DIR_ENTRY_DIR) {
			printk("[DIR ] %s\n", entry.name);
		} else {
			printk("[FILE] %s (%zu bytes)\n", entry.name, entry.size);
		}
	}

	fs_closedir(&dir);
	return ret;
}

static int write_and_verify_file(void)
{
	struct fs_file_t file;
	char read_buf[128];
	const char test_payload[] = "sd ok\n";
	ssize_t length;
	int ret;

	fs_file_t_init(&file);

	ret = fs_open(&file, TEST_FILE_PATH, FS_O_CREATE | FS_O_RDWR);
	if (ret != 0) {
		LOG_ERR("fs_open(write) failed: %d", ret);
		return ret;
	}

	length = fs_write(&file, test_payload, strlen(test_payload));
	if (length < 0) {
		LOG_ERR("fs_write failed: %d", (int)length);
		fs_close(&file);
		return (int)length;
	}

	ret = fs_seek(&file, 0, FS_SEEK_SET);
	if (ret != 0) {
		LOG_ERR("fs_seek failed: %d", ret);
		fs_close(&file);
		return ret;
	}

	memset(read_buf, 0, sizeof(read_buf));
	length = fs_read(&file, read_buf, sizeof(read_buf) - 1);
	if (length < 0) {
		LOG_ERR("fs_read failed: %d", (int)length);
		fs_close(&file);
		return (int)length;
	}

	fs_close(&file);

	printk("Write/Read back: %s", read_buf);
	if (strcmp(read_buf, test_payload) != 0) {
		LOG_ERR("Readback mismatch");
		return -EIO;
	}

	LOG_INF("File write/read verification OK");
	return 0;
}

int main(void)
{
	uint32_t sector_count = 0;
	uint32_t sector_size = 0;
	uint64_t bytes_total;
	int ret;

	LOG_INF("=== SD Card Test ===");

	ret = disk_access_ioctl(DISK_DRIVE_NAME, DISK_IOCTL_CTRL_INIT, NULL);
	if (ret != 0) {
		LOG_ERR("SD init failed: %d", ret);
		return ret;
	}

	ret = disk_access_ioctl(DISK_DRIVE_NAME, DISK_IOCTL_GET_SECTOR_COUNT, &sector_count);
	if (ret != 0) {
		LOG_ERR("GET_SECTOR_COUNT failed: %d", ret);
		return ret;
	}

	ret = disk_access_ioctl(DISK_DRIVE_NAME, DISK_IOCTL_GET_SECTOR_SIZE, &sector_size);
	if (ret != 0) {
		LOG_ERR("GET_SECTOR_SIZE failed: %d", ret);
		return ret;
	}

	bytes_total = (uint64_t)sector_count * sector_size;
	LOG_INF("Sector count: %u", sector_count);
	LOG_INF("Sector size : %u", sector_size);
	LOG_INF("Capacity    : %u MB", (uint32_t)(bytes_total >> 20));

	ret = fs_mount(&mount_info);
	if (ret != 0) {
		LOG_ERR("fs_mount failed: %d", ret);
		return ret;
	}

	LOG_INF("Mounted %s", DISK_MOUNT_PT);

	ret = list_dir(DISK_MOUNT_PT);
	if (ret != 0) {
		fs_unmount(&mount_info);
		return ret;
	}

	ret = write_and_verify_file();
	if (ret != 0) {
		fs_unmount(&mount_info);
		return ret;
	}

	ret = list_dir(DISK_MOUNT_PT);
	if (ret != 0) {
		fs_unmount(&mount_info);
		return ret;
	}

	ret = fs_unmount(&mount_info);
	if (ret != 0) {
		LOG_ERR("fs_unmount failed: %d", ret);
		return ret;
	}

	LOG_INF("SD card test OK");
	return 0;
}