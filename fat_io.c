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

void write2bytes(unsigned char *data, unsigned int number) {
	data[0] = number & 0xff;
	data[1] = (number >> 8) & 0xff;
}

void write4bytes(unsigned char *data, unsigned int number) {
	data[0] = number & 0xff;
	data[1] = (number >> 8) & 0xff;
	data[2] = (number >> 16) & 0xff;
	data[3] = (number >> 24) & 0xff;
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
	fi.begin_cluster = 0xffff;
	fi.size = 0;
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
	if (fileinfo.begin_cluster == 0xffffffff) {
		puts("not found");
	} else {
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
	}

	fclose(disk);
	return 0;
}

void set_fat_sector_by_cluster(FILE* disk, const diskinfo_t *diskinfo,
unsigned char *fat_buffer, unsigned int *fat_sector, int current_cluster) {
	unsigned int next_fat_sector = diskinfo->fat_begin_sector + current_cluster / 0x100;
	unsigned int i;
	if (next_fat_sector != *fat_sector) {
		if (*fat_sector != 0xffffffff) {
			for (i = 0; i < diskinfo->BPB.number_of_fats; i++) {
				write_sector(disk, fat_buffer, *fat_sector + diskinfo->BPB.sectors_per_fat * i);
			}
		}
		*fat_sector = next_fat_sector;
		read_sector(disk, fat_buffer, *fat_sector);
	}
}

int write_disk(const char *diskfile, const char *file, const unsigned char *filename_on_disk) {
	FILE* disk = open_disk(diskfile);
	FILE* input;
	diskinfo_t diskinfo = get_disk_info(disk);
	fileinfo_t fileinfo = search_file_from_root(disk, &diskinfo, filename_on_disk);
	unsigned char rde_buffer[512];
	unsigned int rde_sector = diskinfo.rde_begin_sector - 1;
	unsigned int rde_index = 0xffffffff;
	unsigned int current_cluster = 0;
	unsigned int next_cluster = 0xffffffff;
	unsigned int cluster_to_search = 0;
	unsigned int i;
	if (fileinfo.begin_cluster == 0xffff) {
		puts("creating the file...");
	} else {
		puts("overwriting the file...");
	}

	/* RDEの中からファイルの情報を書き込む位置を探す */
	for (i = 0; i < diskinfo.BPB.root_entries; i++) {
		if (i % 0x10 == 0) {
			read_sector(disk, rde_buffer, ++rde_sector);
		}
		if (fileinfo.begin_cluster == 0xffff) {
			/* 新規作成 */
			unsigned char c = rde_buffer[0x20 * (i % 0x10)];
			if (c == 0x00 || c == 0x05 || c == 0xe5) {
				next_cluster = 0;
				rde_index = i % 0x10;
				break;
			}
		} else {
			/* 上書き */
			if (strncmp((const char*)&rde_buffer[0x20 * (i % 0x10)], (const char*)filename_on_disk, 11) == 0) {
				next_cluster = read2bytes(&rde_buffer[0x20 * (i % 0x10) + 0x1A]);
				rde_index = i % 0x10;
				break;
			}
		}
	}
	if (next_cluster == 0xffffffff) {
		fputs("no space left in RDE\n", stderr);
		fclose(disk);
		return 1;
	}
	/* ファイルの情報を仮設定する */
	for (i = 0; i < 11; i++) rde_buffer[0x20 * rde_index + i] = filename_on_disk[i];
	rde_buffer[0x20 * rde_index + 0x0C] = 0x20;
	rde_buffer[0x20 * rde_index + 0x0D] = 0x00;
	write2bytes(&rde_buffer[0x20 * rde_index + 0x0E], 0); /* createTime */
	write2bytes(&rde_buffer[0x20 * rde_index + 0x10], (35 << 9) | (1 << 5) | 1);
	write2bytes(&rde_buffer[0x20 * rde_index + 0x12], (35 << 9) | (1 << 5) | 1);
	write2bytes(&rde_buffer[0x20 * rde_index + 0x14], 0x0000);
	write2bytes(&rde_buffer[0x20 * rde_index + 0x16], 0);
	write2bytes(&rde_buffer[0x20 * rde_index + 0x18], (35 << 9) | (1 << 5) | 1);
	write2bytes(&rde_buffer[0x20 * rde_index + 0x1A], 0);
	write4bytes(&rde_buffer[0x20 * rde_index + 0x1C], 0);

	input = fopen(file, "rb");
	if (input == NULL) {
		fputs("input file open error\n", stderr);
	} else {
		unsigned char fat_buffer[512];
		unsigned char file_buffer[512];
		unsigned int fat_sector = 0xffffffff;
		unsigned int buffer_size;
		unsigned int file_size = 0;
		unsigned int current_sector = 0;
		unsigned int sector_left = 0;
		while ((buffer_size = fread(file_buffer, 1, 512, input)) > 0) {
			if (sector_left == 0) {
				/* 終端なら、空き領域を探す */
				if (next_cluster == 0) {
					next_cluster = 0xffff;
					for (i = cluster_to_search; i < 0x100 * diskinfo.BPB.sectors_per_fat; i++) {
						if (i % 0x100 == 0) read_sector(disk, fat_buffer, diskinfo.fat_begin_sector + i);
						if (read2bytes(&fat_buffer[2 * (i % 0x100)]) == 0) {
							next_cluster = i;
							break;
						}
					}
					cluster_to_search = next_cluster + 1;
				}
				/* 次のクラスタの情報をFATに書き込む */
				if (current_cluster == 0) {
					write2bytes(&rde_buffer[0x20 * rde_index + 0x1A], next_cluster);
				} else {
					set_fat_sector_by_cluster(disk, &diskinfo, fat_buffer, &fat_sector, current_cluster);
					write2bytes(&fat_buffer[2 * (current_cluster % 0x100)], next_cluster);
				}
				if (next_cluster == 0xffff) {
					fputs("no space left in FAT\n", stderr);
					break;
				}
				/* 次のクラスタの情報を設定する */
				current_cluster = next_cluster;
				current_sector = diskinfo.data_begin_sector + diskinfo.BPB.sectors_per_cluster * current_cluster;
				sector_left = diskinfo.BPB.sectors_per_cluster;
				/* 次の次のクラスタの情報を設定する */
				set_fat_sector_by_cluster(disk, &diskinfo, fat_buffer, &fat_sector, current_cluster);
				next_cluster = read2bytes(&fat_buffer[2 * (current_cluster % 0x100)]);
				if (next_cluster == 0x0001 || 0xFFF7 <= next_cluster) next_cluster = 0;
				printf("current_cluster = 0x%08X, next_cluster = 0x%08X\n", current_cluster, next_cluster);
			}
			printf("writing %u bytes to sector 0x%08X\n", buffer_size, current_sector);
			for (i = buffer_size; i < 512; i++) file_buffer[i] = 0;
			write_sector(disk, file_buffer, current_sector++);
			file_size += buffer_size;
			sector_left--;
		}
		fclose(input);
		/* クラスタチェインを閉じる */
		if (current_cluster != 0) {
			set_fat_sector_by_cluster(disk, &diskinfo, fat_buffer, &fat_sector, current_cluster);
			write2bytes(&fat_buffer[2 * (current_cluster % 0x100)], 0xffff);
		}
		/* ディスクの残りの領域を開放する */
		while (next_cluster != 0) {
			current_cluster = next_cluster;
			set_fat_sector_by_cluster(disk, &diskinfo, fat_buffer, &fat_sector, current_cluster);
			next_cluster = read2bytes(&fat_buffer[2 * (current_cluster % 0x100)]);
			write2bytes(&fat_buffer[2 * (current_cluster % 0x100)], 0x0000);
		}
		if (fat_sector != 0xffffffff) {
			for (i = 0; i < diskinfo.BPB.number_of_fats; i++) {
				write_sector(disk, fat_buffer, fat_sector + diskinfo.BPB.sectors_per_fat * i);
			}
		}
		write4bytes(&rde_buffer[0x20 * rde_index + 0x1C], file_size);
		write_sector(disk, rde_buffer, rde_sector);
	}
	fclose(disk);
	return 0;
}

int delete_disk(const char *diskfile, const unsigned char *filename_on_disk) {
	FILE* disk = open_disk(diskfile);
	diskinfo_t diskinfo = get_disk_info(disk);
	unsigned char rde_buffer[512];
	unsigned char fat_buffer[512];
	unsigned int fat_sector = 0xffffffff;
	unsigned int rde_sector = diskinfo.rde_begin_sector - 1;
	unsigned int current_cluster = 0;
	unsigned int next_cluster = 0;
	unsigned int i;
	for (i = 0; i < diskinfo.BPB.root_entries; i++) {
		if (i % 0x10 == 0) {
			read_sector(disk, rde_buffer, ++rde_sector);
		}
		if (strncmp((const char*)&rde_buffer[0x20 * (i % 0x10)], (const char*)filename_on_disk, 11) == 0) {
			current_cluster = read2bytes(&rde_buffer[0x20 * (i % 0x10) + 0x1A]);
			rde_buffer[0x20 * (i % 0x10)] = 0xe5;
			write_sector(disk, rde_buffer, rde_sector);
			break;
		}
	}
	if (current_cluster == 0) {
		fputs("file not found\n", stderr);
		fclose(disk);
		return 1;
	}
	while (0x0002 <= current_cluster && current_cluster <= 0xfff6) {
		set_fat_sector_by_cluster(disk, &diskinfo, fat_buffer, &fat_sector, current_cluster);
		next_cluster = read2bytes(&fat_buffer[2 * (current_cluster % 0x100)]);
		write2bytes(&fat_buffer[2 * (current_cluster % 0x100)], 0x0000);
		current_cluster = next_cluster;
	}
	if (fat_sector != 0xffffffff) {
		for (i = 0; i < diskinfo.BPB.number_of_fats; i++) {
			write_sector(disk, fat_buffer, fat_sector + diskinfo.BPB.sectors_per_fat * i);
		}
	}
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
		if (out_pos <= 8 && argv[4][i] == '.') {
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
		return delete_disk(argv[1], filename_on_disk);
	} else {
		fputs("unknown operation\n", stderr);
		return 1;
	}
}
