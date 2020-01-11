/*********************************************************************************
* JesFsHex2Bin - ('Intel-Hex' to Binary Conversion)
*
* Written to convert/assemble HEX files for ARM into Binary Files, optionally
* remove unused parts and add with a bootable header. Part of JesFs Bootloader
*
* (C) JoEmbedded.de
*
* Version:
* 1.00	/ 11.01.2020
*********************************************************************************/

#define VERSION "1.00 / 11.01.2020"

#define _CRT_SECURE_NO_WARNINGS // For VisualStudio
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <assert.h>
#include <time.h>

#define MAX_BUF	2048*1024	// 2MB fix

#define BINDEF_VAL	0xFF	// Binary Default Value of empty Memory
uint8_t binbuf[MAX_BUF];	// Binary Data, init with BINDEF_VAL
uint8_t usedbuf[MAX_BUF];	// Counts use, init with 0 by default
int		min_bin_addr = MAX_BUF-1;	// Used Addresses
int		max_bin_addr = 0;
int		bin_bytes_cnt;

#define MAXLINE	120
char in_line[MAXLINE + 1];
int in_line_cnt, total_line_cnt;

#define MAX_WARN	10		// Maximum displayed Warnings
int warnings_cnt;

int lowest_output_addr = -1;

static char* pbyte;
static uint8_t fcs;
int getbyte(void){
	int val = 0,n;
	for (int i = 0; i < 2; i++) {
		n = *pbyte++;
		if (n >= '0' && n <= '9') n -= '0';
		else if (n >= 'a' && n <= 'f') n = n- 'a' + 10;
		else if (n >= 'A' && n <= 'F') n = n- 'A' + 10;
		else return -1;	// Unknown
		val <<= 4;
		val += n;
	}
	fcs += val;
	return val;
}

int getuint16(void) {
	int ret16h, ret16l;
	ret16h = getbyte();
	if (ret16h < 0) return -1;
	ret16l = getbyte();
	if (ret16l < 0) return -1;
	return (ret16h << 8) + ret16l;
}

/* Write 1 Byte to Buffer */
int write_byte(int addr, int val) {
	uint8_t ubc;	// Used Buffer Counter, should be 0
	if (addr < 0 || addr >= MAX_BUF) {
		return -1;
	}
	ubc = usedbuf[addr];
	if (ubc) {
		if (warnings_cnt++ < MAX_WARN) {
			printf("WARNING: Overwriting Memory at Addr: 0x%X",addr);
		}
	}
	if (ubc < 255) usedbuf[addr] = ubc + 1; // Mark usage / color array
	binbuf[addr] = val;	// Save Value
	if (addr > max_bin_addr) max_bin_addr = addr;	// Save Bounds
	if(addr < min_bin_addr) min_bin_addr = addr;
	bin_bytes_cnt++;	// Count this input
	return 0; // Write OK
}

int read_infile(char* infilename) {
	FILE* inf;
	char* pc;
	int rtyp;
	int rlen;
	int bval;
	int badr = 0; // 16 Bit Address (before data)
	int boffset = 0; // 32 Bit Offset for following data
	int init_seg, init_lu16, init_hu16;
	inf = fopen(infilename, "r");
	if (!inf) {
		printf("ERROR: Can't open '%s'\n", infilename);
		return -1;
	}
	printf("Input File '%s'\n", infilename);
	in_line_cnt = 0;
	for (;;) {
		if (!fgets(in_line, MAXLINE, inf)) {
			printf("ERROR: Unexpected File End in Line %d\n", in_line_cnt);
			return -2;
		}
		pc = in_line;
		if (*pc++ != ':') {
			printf("ERROR: Missing ':' in Line %d\n", in_line_cnt);
			return -3;
		}
		pbyte = pc;
		fcs = 0;
		rlen = getbyte();
		if (rlen < 0) {
			printf("ERROR: Read Len in Line %d\n", in_line_cnt);
			return -7;
		}
		badr = getuint16();
		if (badr < 0) {
			printf("ERROR: Read Adr.16 in Line %d\n", in_line_cnt);
			return -8;
		}

		rtyp = getbyte();
		switch (rtyp) {
		case 0:	// Data Record
			while (rlen--) {
				bval = getbyte();
				if (write_byte(badr+boffset, bval)) {
					printf("ERROR: Typ:%02X - Illegal Write(Addr: 0x%X) in Line %d\n", rtyp, badr, in_line_cnt);
					return -5;
				}
				badr++;
			}
			getbyte();
			if (fcs) {
				printf("ERROR: Typ:%02X - FCS Error in Line %d\n", rtyp, in_line_cnt);
				return -6;
			}
			break;
		case 1: // End
			if (getbyte() != 255) {
				printf("ERROR: Typ:%02X - End-Record, missing 'FF' in Line %d\n", rtyp, in_line_cnt);
				return -4;
			}
			fclose(inf);
			return 0;	// Regular Return, NO ERROR

		case 2:	// extended segment address record (added as '<<4') in Segment-Form
			// *** Maximum Address Range is 1MB
			boffset = getuint16();
			if (boffset < 0) {
				printf("ERROR: Typ:%02X - Read Extended Segment in Line %d\n", rtyp, in_line_cnt);
				return -9;
			}
			//printf("Segment 0x%0X\n", boffset);
			boffset <<= 4;	// Make it upper.4 of u32
			break;

		case 3:	// Init-Addr in Segment-Form
			// *** Maximum Address Range is 1MB
			init_seg= getuint16();
			init_lu16= getuint16();
			if (init_seg < 0 || init_lu16<0) {
				printf("ERROR: Typ:%02X - Read Init Address in Line %d\n", rtyp, in_line_cnt);
				return -10;
			}
			printf("Info: Init Address: 0x%X\n", (init_seg << 4) + init_lu16);
			break;

		case 4:	// Upper 16 Bit of Address (linear)
			// *** Maximum Address Range is 2GB
			boffset = getuint16();
			if (boffset < 0) {
				printf("ERROR: Typ:%02X - Read Offset in Line %d\n", rtyp, in_line_cnt);
				return -9;
			}
			boffset <<= 16;	// Make it upper.16 of u32
			//printf("Offset 0x%X\n",boffset);
			break;

		case 5:	// Init-Addr in Linear.32 Form
			// *** Maximum Address Range is 2GB
			init_hu16 = getuint16();
			init_lu16 = getuint16();
			if (init_hu16 < 0 || init_lu16 < 0) {
				printf("ERROR: Typ:%02X - Read Init Address in Line %d\n", rtyp, in_line_cnt);
				return -11;
			}
			printf("Info: Init Address: 0x%X\n", (init_hu16 << 16) + init_lu16);
			break;

		default:
			printf("ERROR: Typ:%02X - Unknown in Line %d\n", rtyp, in_line_cnt);
			return -5;
		}

		in_line_cnt++;
		total_line_cnt++;
	}
}
/* Same as JesFs CRC32: Calculating a CRC32: Also useful for external use */
#define POLY32 0xEDB88320 // ISO 3309
uint32_t fs_track_crc32(uint8_t* pdata, uint32_t wlen, uint32_t crc_run) {
	uint8_t j;
	while (wlen--) {
		crc_run ^= *pdata++;
		for (j = 0; j < 8; j++) {
			if (crc_run & 1)
				crc_run = (crc_run >> 1) ^ POLY32;
			else
				crc_run = crc_run >> 1;
		}
	}
	return crc_run;
}

#define HDR0_MAGIC	0xE79B9C4F
// Definition for Headers
typedef struct {
	uint32_t hdrmagic;   // 0 MagicHeader Type0: HDR0_MAGIC
	uint32_t hdrsize;	 // 1 Size in Bytes (Type0: 32 for 8 uint32)
	uint32_t binsize;	 // 2 Size of following BinaryBlock
	uint32_t binload;	 // 3 Adr0 of following BinaryBlock
	uint32_t crc32;		 // 4 CRC32 of following BinaryBlock
	uint32_t timestamp;	 // 5 UnixSeconds of this file
	uint32_t binary_start; // 6 StartAddress Binary (Parameter 2 of 'h')
	uint32_t resv0;		 // 7 Reserved, 0xFFFFFFFF
} HDR0_TYPE;

/* Write the opt. Header to outf */
int write_header(FILE* outf, int hdrtype, int min_bin_addr, int anz, uint32_t par1) {
	uint32_t crc32 = fs_track_crc32(&binbuf[min_bin_addr], anz, 0xFFFFFFFF);
	//printf("CRC32: %08X\n", crc32);
	HDR0_TYPE hdr0;

	switch (hdrtype) {
	case 0:
		assert(sizeof(hdr0) == 32);
		hdr0.hdrmagic = HDR0_MAGIC;
		hdr0.hdrsize = 32;	
		hdr0.binsize = anz;
		hdr0.binload = min_bin_addr;
		hdr0.crc32 = crc32;	
		hdr0.timestamp = (uint32_t)time(NULL);	// now()
		hdr0.binary_start = par1;	// Start-Addres of Binary (Vectortable) (e.g. 0 or after Softdevice)
		hdr0.resv0 = 0xFFFFFFFF;
		if (fwrite(&hdr0, 1, sizeof(hdr0), outf) != sizeof(hdr0)) {
			printf("ERROR: File Write Error!\n");
			return -20;
		}
		printf("Header Type 0: Vector Table of Binary: 0x%X\n", par1);
		printf("Timestamp: 0x%X\n", hdr0.timestamp);

		break;
	default:
		printf("ERROR: Unknown Header Type '%d'\n", hdrtype);
		return -19;
	}
	return 0; // Hdr. OK	
}


//------- MAIN -----------
int main(int argc, char** argv) {
	FILE* outf;
	int res=0,i,anz,hdrtype=-1;
	uint32_t par1 = 0;
	char* pc;
	char* infilename = NULL;
	char* outfilename = NULL;
	printf("*** JesFsHex2Bin " VERSION " (C)JoEmbedded.de\n\n");
	memset(binbuf, BINDEF_VAL, MAX_BUF);
		
	if (argc <= 1) {
		printf("Usage: FILE1.HEX [FILE2.HEX ...] [-cLOW_ADDR] [-hHDRTYPE] [-oOUTFILE.BIN]\n\n");

		printf("Combines all .HEX-files in OUTFILE.BIN\n");
		printf("If LOW_ADDR is set, only Bytes at Addr. >= LOW_ADDR will be written,\n");
		printf("else use lowest Addr. as first Output Byte. Format: Dec. or 0x.. for Hex.\n");
		printf("HDRTYPE specifies optional (leading) Header to Binary (see Docu).\n\n");
		return -13;
	}

	for (i = 1; i < argc; i++) {
		if (*argv[i] == '-') {
			switch (*(argv[i] + 1)) {
			case 'c':
				lowest_output_addr = strtoul(argv[i] + 2, 0, 0);
				break;
			case 'h':
				pc = argv[i] + 2;
				hdrtype = strtoul(pc, &pc, 0);
				if (!*pc) break;
				if (*pc++ != ',') {
					printf("ERROR: Option Format!\n");
					return -21;
				}
				par1 = strtoul(pc, 0, 0);	// Start-Addr of Binary
				break;
			case 'o':
				outfilename = argv[i] + 2;
				if (!strlen(outfilename)) {
					printf("ERROR: No Outfile Name\n");
					return -15;
				}
				break;
			default:
				printf("ERROR: Unknown Option '%s'\n", argv[i]);
				return -14;
			}
		}else {
			infilename = argv[i];
			res = read_infile(infilename);
			if (res) {
				break;
			}else {
				printf("Input File '%s' OK, %d lines\n", infilename, in_line_cnt);
			}
		}
	}

	if (warnings_cnt) {
		printf("*** %d Warnings found ***\n", warnings_cnt);
	}
	if (!res) {
		if (bin_bytes_cnt == 0) {
			printf("ERROR: No or empty Input Files\n");
			res = -12;
		}else {
			printf("OK. Input %d Bytes (Addr: 0x%X...0x%X) Total: %d lines\n", bin_bytes_cnt, min_bin_addr, max_bin_addr, total_line_cnt);
			/*
			for (anz = 0, i = min_bin_addr; i <= max_bin_addr; i++) if (!usedbuf[i]) anz++;	printf("(Check: %d unused Bytes)\n", anz);
			for (anz = 0, i = min_bin_addr; i <= max_bin_addr; i++) if (usedbuf[i]==1) anz++;	printf("(Check: %d 1-time used Bytes)\n", anz);
			*/
			if (outfilename) {
				if (lowest_output_addr >= 0) min_bin_addr = lowest_output_addr;
				anz = max_bin_addr - min_bin_addr+1;	// min_bin_addr is last written addr
				if (anz <= 0) {
					printf("ERROR: No Data to Write\n");
					return -16;
				}
				printf("Write '%s', %d Bytes (Addr: 0x%X...0x%X)\n", outfilename, anz, min_bin_addr, max_bin_addr);
				outf = fopen(outfilename, "wb");
				if (!outf) {
					printf("ERROR: Can't open '%s'\n", outfilename);
					return -17;
				}
				if (hdrtype >= 0) {
					res = write_header(outf, hdrtype, min_bin_addr, anz, par1);
					if (res) return res;
				}

				if (fwrite(&binbuf[min_bin_addr], 1, anz, outf) != anz) {
					printf("ERROR: Write Error '%s'\n", outfilename);
					res = -18;
				}
				fclose(outf);
			}

		}
	}
	return res;
	
}
// ***
