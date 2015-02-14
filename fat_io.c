#include <stdio.h>
#include <stdlib.h>
#include <string.h>

FILE* open_disk(const char *name) {
	FILE* fp = fopen(name, "rb+");
	if (fp == NULL) {
		fputs("disk open error\n", stderr);
		exit(1);
	}
	return fp;
}

void read_sector(FILE* fp, unsigned char *sector, unsigned int sector_idx) {
	fseek(fp, 512 * sector_idx, SEEK_SET);
	fread(sector, 512, 1, fp);
}

void write_sector(FILE* fp, const unsigned char *sector, unsigned int sector_idx) {
	fseek(fp, 512 * sector_idx, SEEK_SET);
	fwrite(sector, 512, 1, fp);
}

unsigned int read2bytes(const unsigned char *data) {
	return (data[1] << 8) | data[0];
}

unsigned int read4bytes(const unsigned char *data) {
	return (data[3] << 24) | (data[2] << 16) | (data[1] << 8) | data[0];
}

typedef struct {
	struct {
		unsigned int first_sector;
		unsigned int partition_size;
	} MBR;
	struct {
		unsigned int bytes_per_sector;
		unsigned int sectors_per_cluster;
		unsigned int reserved_sectors;
		unsigned int number_of_fats;
		unsigned int root_entries;
		unsigned int total_sectors;
		unsigned int sectors_per_fat;
		unsigned int hidden_sectors;
	} BPB;
	unsigned int fat_begin_sector;
	unsigned int rde_begin_sector;
	unsigned int data_begin_sector;
} diskinfo_t;

typedef struct {
	unsigned int begin_cluster;
	unsigned int size;
} fileinfo_t;

diskinfo_t get_disk_info(FILE* fp) {
	unsigned char buf[512];
	diskinfo_t di;
	read_sector(fp, buf, 0);
	di.MBR.first_sector = read4bytes(&buf[0x1C6]);
	di.MBR.partition_size = read4bytes(&buf[0x1CA]);

	read_sector(fp, buf, di.MBR.first_sector);
	di.BPB.bytes_per_sector = read2bytes(&buf[0x0B]);
	di.BPB.sectors_per_cluster = buf[0x0D];
	di.BPB.reserved_sectors = read2bytes(&buf[0x0E]);
	di.BPB.number_of_fats = buf[0x10];
	di.BPB.root_entries = read2bytes(&buf[0x11]);
	di.BPB.total_sectors = read2bytes(&buf[0x13]);
	if (di.BPB.total_sectors == 0) di.BPB.total_sectors = read4bytes(&buf[0x20]);
	di.BPB.sectors_per_fat = read2bytes(&buf[0x16]);
	di.BPB.hidden_sectors = read4bytes(&buf[0x1C]);

	di.fat_begin_sector = di.BPB.hidden_sectors + di.BPB.reserved_sectors;
	di.rde_begin_sector = di.fat_begin_sector + di.BPB.sectors_per_fat * di.BPB.number_of_fats;
	di.data_begin_sector = di.rde_begin_sector + di.BPB.root_entries / 0x10 - di.BPB.sectors_per_cluster * 2;
	return di;
}

fileinfo_t search_file_from_root(FILE* fp, const diskinfo_t* di, const unsigned char *filename) {
	unsigned char buf[512];
	unsigned int i;
	unsigned int sector = di->rde_begin_sector;
	fileinfo_t fi;
	fi.begin_cluster = fi.size = 0;
	for (i = 0; i < di->BPB.root_entries; i++) {
		int j, ok = 1;
		if (i % 0x10 == 0) read_sector(fp, buf, sector++);
		for (j = 0; j < 11; j++) {
			if (buf[0x20 * (i % 0x10) + j] != filename[j]) {
				ok = 0;
				break;
			}
		}
		if (ok) {
			fi.begin_cluster = read2bytes(&buf[0x20 * (i % 0x10) + 0x1A]);
			fi.size = read4bytes(&buf[0x20 * (i % 0x10) + 0x1C]);
			break;
		}
	}
	return fi;
}

int read_disk(const char *diskfile, const char *file, const unsigned char *filename_on_disk) {
	FILE* disk = open_disk(diskfile);
	FILE* out;
	diskinfo_t diskinfo = get_disk_info(disk);
	fileinfo_t fileinfo = search_file_from_root(disk, &diskinfo, filename_on_disk);
	puts("----- disk info -----");
	printf("total_sectors     = 0x%08X (%u bytes)\n", diskinfo.BPB.total_sectors, 0x200 * diskinfo.BPB.total_sectors);
	printf("fat_begin_sector  = 0x%08X (0x%08X bytes)\n", diskinfo.fat_begin_sector, 0x200 * diskinfo.fat_begin_sector);
	printf("rde_begin_sector  = 0x%08X (0x%08X bytes)\n", diskinfo.rde_begin_sector, 0x200 * diskinfo.rde_begin_sector);
	printf("data_begin_sector = 0x%08X (0x%08X bytes)\n", diskinfo.data_begin_sector, 0x200 * diskinfo.data_begin_sector);
	puts("----- file info -----");
	printf("begin_cluster     = 0x%08X\n", fileinfo.begin_cluster);
	printf("size              = 0x%08X (%u)\n", fileinfo.size, fileinfo.size);

	out = fopen(file, "wb");
	if (out == NULL) {
		fputs("output file open error\n", stderr);
	} else {
		unsigned char fat_buffer[512];
		unsigned char file_buffer[512];
		unsigned int loaded_fat = 0xffffffff;
		unsigned int current_cluster = fileinfo.begin_cluster;
		unsigned int size_left = fileinfo.size;
		puts("----- file read -----");
		while (size_left > 0) {
			unsigned int i;
			unsigned int current_sector = diskinfo.data_begin_sector + diskinfo.BPB.sectors_per_cluster * current_cluster;
			unsigned int fat_index;
			/* read this cluster */
			printf("reading cluster 0x%08X\n", current_cluster);
			for (i = 0; size_left > 0 && i < diskinfo.BPB.sectors_per_cluster; i++) {
				read_sector(disk, file_buffer, current_sector++);
				fwrite(file_buffer, size_left > 0x200 ? 0x200 : size_left, 1, out);
				if (size_left < 0x200) size_left = 0; else size_left -= 0x200;
			}
			/* go to next cluster */
			fat_index = current_cluster / 0x100;
			if (loaded_fat != fat_index) {
				read_sector(disk, fat_buffer, diskinfo.fat_begin_sector + fat_index);
				loaded_fat = fat_index;
			}
			current_cluster = read2bytes(&fat_buffer[(current_cluster % 0x100) * 2]);
			if (size_left > 0 && (current_cluster <= 0x0001 || 0xfff7 <= current_cluster)) {
				fputs("ERROR: FAT chain is broken in the middle of the file.\n", stderr);
			} else if (size_left == 0 && current_cluster <= 0xfff7) {
				fputs("WARNING: FAT chain doesn't end at the end of the file.\n", stderr);
			}
		}
		fclose(out);
	}

	fclose(disk);
	return 0;
}

int write_disk(const char *diskfile, const char *file, const unsigned char *filename_on_disk) {
	FILE* disk = open_disk(diskfile);
	diskinfo_t diskinfo = get_disk_info(disk);
	fclose(disk);
	return 0;
}

int delete_disk(const char *diskfile, const char *file, const unsigned char *filename_on_disk) {
	FILE* disk = open_disk(diskfile);
	diskinfo_t diskinfo = get_disk_info(disk);
	fclose(disk);
	return 0;
}

int main(int argc, char *argv[]) {
	unsigned char filename_on_disk[12];
	int i;
	int out_pos;
	if (argc != 5) {
		fprintf(stderr, "Usage: %s disk_file operation file filename_on_disk\n", argc>0?argv[0]:"fat_io");
		return 1;
	}
	for (i = 0; i < 12; i++) filename_on_disk[i] = 0x20;
	for (i = out_pos = 0; argv[4][i] != '\0'; i++) {
		if (out_pos < 8 && argv[4][i] == '.') {
			out_pos = 8;
		} else {
			if (out_pos < 11) filename_on_disk[out_pos++] = (unsigned char)argv[4][i];
		}
	}
	if (strcmp(argv[2], "read") == 0) {
		return read_disk(argv[1], argv[3], filename_on_disk);
	} else if (strcmp(argv[2], "write") == 0) {
		return write_disk(argv[1], argv[3], filename_on_disk);
	} else if (strcmp(argv[2], "delete") == 0) {
		return delete_disk(argv[1], argv[3], filename_on_disk);
	} else {
		fputs("unknown operation\n", stderr);
		return 1;
	}
}