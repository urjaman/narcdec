/*   Nokia S60 (mostly 6630) Backup.Arc decompressor.
 *   Copyright (C) 2013  Urja Rannikko <urjaman@gmail.com>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/* Without iconv we will save filenames in Latin1, with iconv we can do 
   UTF-8 (or other local encoding). */

#define USE_ICONV
#define USE_MMAP
//#define PRINT_UNKNOWN_METADATA

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <libgen.h>
#ifdef USE_MMAP
#include <sys/mman.h>
#endif
#ifdef USE_ICONV
#include <iconv.h>
#include <langinfo.h>
#include <locale.h>
#endif
#include <setjmp.h>
#include <zlib.h>

int arcfile_fd = -1;
void * arcfile_data = NULL;
off_t arcfile_size = 0;
/* This is a virtual offset in memory. */
off_t arcfile_offs = 0;
/* This counts how much of it all we understood... */
off_t arcfile_understood = 0;
int arcfile_files_extracted = 0;
static jmp_buf fail_exit;

void die(const char* why)
{
	perror(why);
	exit(1);
}

void mdie(const char *why)
{
	fprintf(stderr,"die: %s\n",why);
	exit(2);
}

void open_map_file(const char* filename)
{
	/* Open and somehow map the file to be visible in 
	   arcfile_data. If porting to a place where mmap() doesnt work,
	   malloc + copy is fine.
	*/
	arcfile_fd = open(filename, O_RDONLY);
	if (arcfile_fd < 0) die("open");	
	
	off_t filesize = lseek(arcfile_fd, 0, SEEK_END);
	if (filesize == (off_t)-1) die("lseek");
	arcfile_size = filesize;
	
#ifdef USE_MMAP
	arcfile_data = mmap(0, filesize, PROT_READ, MAP_SHARED, 
				arcfile_fd, 0);
	if (arcfile_data == MAP_FAILED) die("mmap");
#else
	arcfile_data = malloc(filesize);
	if (!arcfile_data) die("malloc");
	if (lseek(arcfile_fd, 0, SEEK_SET) != 0) die("lseek");
	off_t read_offset = 0;
	do {
		ssize_t rv = read(arcfile_fd, arcfile_data+read_offset, 
					filesize - read_offset);
		if (rv<=0) die("read");
		read_offset += rv;
	} while (read_offset < filesize);
#endif
}

/* Note: Little-Endian only. */
uint32_t ru32(void) {
	off_t o = arcfile_offs;
	arcfile_offs += sizeof(uint32_t);
	if (arcfile_offs>arcfile_size) mdie("read past file end");
	return *(uint32_t*)(arcfile_data+o);
}

uint32_t ru32f(void) {
	off_t o = arcfile_offs;
	arcfile_offs += sizeof(uint32_t);
	if (arcfile_offs>arcfile_size) longjmp(fail_exit,1);
	return *(uint32_t*)(arcfile_data+o);
}

uint8_t ru8f(void) {
	off_t o = arcfile_offs;
	arcfile_offs += sizeof(uint8_t);
	if (arcfile_offs>arcfile_size) longjmp(fail_exit,1);
	return *(uint8_t*)(arcfile_data+o);
}

void vseekf(int amnt) {
	off_t no = arcfile_offs + amnt;
	if ((no < 0) || (no > arcfile_size)) longjmp(fail_exit,1);
	arcfile_offs = no;
}

char* read_unicode_stringf(int len)
{
	/* We generate ISO8859-1 / Latin-1.
	  In case of non-Latin1 characters we signal failure. */
	char * buf = malloc(len+1);
	if (!buf) die("malloc");
	for (int i=0;i<len;i++) {
		buf[i] = ru8f();
		if (ru8f() != 0) longjmp(fail_exit,1);
	}
	buf[len] = 0;
	return buf;
}

char * make_unix_pathname(char * org_fn) 
{
	char tmp[256];
	strcpy(tmp,org_fn);

	/* 1st: remove any /'s */
	for (int i=0;tmp[i];i++) {
		if (tmp[i] == '/') tmp[i] = '_';
	}
	/* 2nd: change \'s to /'s */
	for (int i=0;tmp[i];i++) {
		if (tmp[i] == '\\') tmp[i] = '/';
	}

#ifdef USE_ICONV
	char tmpout[256];
	char to_code[64];
	strcpy(to_code,nl_langinfo(CODESET));
	strcat(to_code,"//TRANSLIT");
	iconv_t cd = iconv_open(to_code, "ISO8859-1");
	if (!cd) die("iconv_open");
	size_t inbl=strlen(tmp),outbl=255;
	char *inbuf=&(tmp[0]), *outbuf=&(tmpout[0]);
	memset(tmpout,0,256);
	if (iconv(cd,&inbuf,&inbl,&outbuf,&outbl)==(size_t)-1) die("iconv");
	char * rv = malloc(strlen(tmpout)+1);
	if (!rv) die("malloc");
	strcpy(rv,tmpout);
	iconv_close(cd);
#else
	char *rv = malloc(strlen(tmp)+1);
	if (!rv) die("malloc");
	strcpy(rv,tmp);
#endif
	return rv;
}	

void make_directory(char *dir)
{
	char buf[128];
	char * t = dir;
	int depth=0;	
	while (1) {
		char *e = strstr(t,"/");
		if (e) {
			memcpy(buf,t,e-t);
			buf[e-t] = 0;
		} else {
			strcpy(buf,t);
		}
		if (strcmp(buf,".")==0) {
			/* This isnt doing anything useful ... */		
		} else {
			mkdir(buf,0777);
			if (chdir(buf)<0) die("mkdir+chdir");
			depth++;
		}
		if (e) {
			t = e+1;
		} else {
			break;
		}
	}
	for (int i=0;i<depth;i++) chdir("..");
}
	

void output_compressed_file(const char* name, uint32_t lenComp, uint32_t lenUnc)
{
	if ((arcfile_offs+lenComp) > (uint32_t)arcfile_size) longjmp(fail_exit,1);
	char tmp[256];
	strcpy(tmp,name);
	make_directory(dirname(tmp));
	int cdfd = open(name, O_CREAT|O_WRONLY, 0666);
	if (cdfd<0) die("open");
	if (lenUnc!=0) { // Zero-len files dont need any of this
		uint8_t *obuf = malloc(lenUnc);
		if (!obuf) die("malloc");
		z_streamp strm = calloc(1,sizeof(z_stream));
		if (!strm) die("calloc");
		strm->next_in = arcfile_data+arcfile_offs;
		strm->avail_in = lenComp;
		strm->next_out = obuf;
		strm->avail_out = lenUnc;
		if (inflateInit(strm)!=Z_OK) mdie("inflateInit");
		if (inflate(strm,Z_FINISH)!=Z_STREAM_END) mdie("inflate");
		inflateEnd(strm);
		free(strm);
		if (write(cdfd,obuf,lenUnc)!=(ssize_t)lenUnc) die("write");
		free(obuf);
	}
	close(cdfd);
	arcfile_offs += lenComp;
}

int extract_arc_file_try(void)
{
	if (setjmp(fail_exit)) {
		return 1; /* Failure :/ */
	}
	int len = ru8f();
	if (len&1) return 1;
	len = len / 2;
	char * filename = read_unicode_stringf(len);
	char * upthname = make_unix_pathname(filename);
	free(filename);
#ifdef PRINT_UNKNOWN_METADATA
	printf("Unknown metadata: ");
	for (int i=0;i<10;i++) {
		uint8_t c = ru8f();
		printf("%02X ",c);
	}
	printf("\n");
#else
	vseekf(10); // "?? datetime?"
#endif
	uint32_t lenUncomp = ru32f();
	uint32_t lenComp = ru32f();
	/* Sanity checks. */
	if (lenComp > (uint32_t)arcfile_size) return 1;
	if ((lenUncomp/100) > (uint32_t)arcfile_size) return 1; 
	printf("%s - size: %u / %u\n",upthname,lenComp, lenUncomp);
	output_compressed_file(upthname,lenComp,lenUncomp);
	free(upthname);
	arcfile_understood += 1 + (len*2) + 10 + 8 + lenComp;
	arcfile_files_extracted++;
	return 0; 
}

void extract_arc_files_main(void) 
{
	arcfile_files_extracted = 0;
	arcfile_understood = 0;
	arcfile_offs = 0; /* RESET */
	
	uint32_t test0 = ru32();
	if (test0 == 0x101F4667) {
		mdie("this format not yet implemented");
	} else {
		const char pathStartSeq[5] = { 0, ':', 0, '\\', 0 };
		const uint8_t *dp;
		reset_parse:
		if ((dp=memmem(arcfile_data+arcfile_offs,arcfile_size-arcfile_offs,
			pathStartSeq, 5))) {
			arcfile_offs = dp - (uint8_t*)arcfile_data;
			arcfile_offs -= 2;
			while (1) {
				if (extract_arc_file_try()) {
					if (arcfile_offs>=arcfile_size) {
						break; /* End Of File */
					} else {
						goto reset_parse;
					}
				}
			}
		}
	}
}

int main(int argc, char** argv) 
{

#ifdef USE_ICONV
	setlocale(LC_ALL, "");
#endif

	if (argc != 2) {
		fprintf(stderr,"usage: narcdec <file>\n");
		return 1;
	} else {
		open_map_file(argv[1]);
		extract_arc_files_main();
		/* 16 is normal, and we might add header parsing so.. */
		if ((arcfile_size - arcfile_understood) <= 16) {
			printf("Done %d files.\n",
				arcfile_files_extracted);
		} else {
			printf("Done %d files. %d bytes not understood\n",
				arcfile_files_extracted,
				(int)(arcfile_size - arcfile_understood));
		}
		return 0;
	}	
}
