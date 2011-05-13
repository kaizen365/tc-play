/*
 * Copyright (c) 2011 The DragonFly Project.  All rights reserved.
 *
 * This code is derived from software contributed to The DragonFly Project
 * by Alex Hornung <ahornung@gmail.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name of The DragonFly Project nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific, prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <sys/endian.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <err.h>
#include <uuid.h>
#include <termios.h>
#include <libdevmapper.h>
#include <openssl/evp.h>

#include "crc32.h"
#include "tc-play.h"

/* XXX TODO:
 *  - LRW-benbi support? needs further work in dm-crypt and even opencrypto
 *  - secure buffer review (i.e: is everything that needs it using secure mem?)
 *  - mlockall? (at least MCL_FUTURE, which is the only one we support)
 */

#if 0
/* Volume times:
 * return wxDateTime ((time_t) (volumeTime / 1000ULL / 1000 / 10 - 134774ULL * 24 * 3600));
 */
#define VOLTIME_TO_TIME(volumeTime) \
    ((time_t)(volumeTime / 1000ULL / 1000 / 10 - 134774ULL * 24 * 3600))

/*
Hi, dm-params:
0 261632 crypt aes-xts-plain64                                                                                                                                0 256 /dev/loop0 256

Hi, dm-params: 0 261632 crypt aes-xts-plain64 0 256 /dev/loop0 256
				256 = off_mk_scope / sec_sz!
		261632 = sz_mk_scope / sec_sz!

	 aes-cbc-essiv:sha256 7997f8af... 0 /dev/ad0s0a 8 
			         iv off---^  block off--^ 

Volume "/home/alex/tc-play/tctest.container" has been mounted.
*/

#endif

/* Version of tc-play */
#define MAJ_VER		0
#define MIN_VER		2

/* Comment out to disable debug info */
#define DEBUG		1

/* Endianess macros */
#define BE_TO_HOST(n, v) v = be ## n ## toh(v)
#define LE_TO_HOST(n, v) v = le ## n ## toh(v)


/* Supported algorithms */
struct pbkdf_prf_algo pbkdf_prf_algos[] = {
	{ "RIPEMD160",	2000 },
	{ "RIPEMD160",	1000 },
	{ "SHA512",	1000 },
	{ "whirlpool",	1000 },
	{ NULL,		0    }
};

struct tc_crypto_algo tc_crypto_algos[] = {
	{ "AES-128-XTS",	"aes-xts-plain",	32,	8 },
	{ "AES-256-XTS",	"aes-xts-plain",	64,	8 },
	{ NULL,			NULL,			0,	0 }
};

int
hex2key(char *hex, size_t key_len, unsigned char *key)
{
	char hex_buf[3];
	size_t key_idx;
	hex_buf[2] = 0;
	for (key_idx = 0; key_idx < key_len; ++key_idx) {
		hex_buf[0] = *hex++;
		hex_buf[1] = *hex++;
		key[key_idx] = (unsigned char)strtoul(hex_buf, NULL, 16);
	}
	hex_buf[0] = 0;
	hex_buf[1] = 0;

	return 0;
}

#ifdef DEBUG
void
print_hex(unsigned char *buf, off_t start, size_t len)
{
	size_t i;

	for (i = start; i < start+len; i++)
		printf("%02x", buf[i]);

	printf("\n");
}
#endif

void *
alloc_safe_mem(size_t req_sz)
{
	struct safe_mem_hdr *hdr;
	struct safe_mem_tail *tail;
	size_t alloc_sz;
	void *mem, *user_mem;

	alloc_sz = req_sz + sizeof(*hdr) + sizeof(*tail);
	if ((mem = malloc(alloc_sz)) == NULL)
		return NULL;

	if (mlock(mem, alloc_sz) < 0) {
		free(mem);
		return NULL;
	}

	memset(mem, 0, alloc_sz);

	hdr = (struct safe_mem_hdr *)mem;
	tail = (struct safe_mem_tail *)(mem + alloc_sz - sizeof(*tail));
	user_mem = mem + sizeof(*hdr);

	strcpy(hdr->sig, "SAFEMEM");
	strcpy(tail->sig, "SAFEMEM");
	hdr->alloc_sz = alloc_sz;

	return user_mem;
}

void
free_safe_mem(void *mem)
{
	struct safe_mem_hdr *hdr;
	struct safe_mem_tail *tail;

	mem -= sizeof(*hdr);
	hdr = (struct safe_mem_hdr *)mem;
	tail = (struct safe_mem_tail *)(mem + hdr->alloc_sz - sizeof(*tail));

	/* Integrity checks */
	if ((memcmp(hdr->sig, "SAFEMEM\0", 8) != 0) ||
	    (memcmp(tail->sig, "SAFEMEM\0", 8) != 0)) {
		fprintf(stderr, "BUG: safe_mem buffer under- or overflow!!!\n");
		exit(1);
	}

	memset(mem, 0, hdr->alloc_sz);

	free(mem);
}

void *
read_to_safe_mem(const char *file, off_t offset, size_t *sz)
{
	void *mem = NULL;
	int fd;

	if ((fd = open(file, O_RDONLY)) < 0) {
		fprintf(stderr, "Error opening file %s\n", file);
		return NULL;
	}

	if ((mem = alloc_safe_mem(*sz)) == NULL) {
		fprintf(stderr, "Error allocating memory\n");
		goto out;
	}

	if ((lseek(fd, offset, SEEK_SET) < 0)) {
		fprintf(stderr, "Error seeking on file %s\n", file);
		goto m_err;
	}

	if ((*sz = read(fd, mem, *sz)) <= 0) {
		fprintf(stderr, "Error reading from file %s\n", file);
		goto m_err;
	}

out:
	close(fd);
	return mem;
	/* NOT REACHED */

m_err:
	free_safe_mem(mem);
	close(fd);
	return NULL;
}

int
tc_encrypt(const char *cipher_name, unsigned char *key, unsigned char *iv,
    unsigned char *in, int in_len, unsigned char *out)
{
	const EVP_CIPHER *evp;
	EVP_CIPHER_CTX ctx;
	int outl, tmplen;

	evp = EVP_get_cipherbyname(cipher_name);
	if (evp == NULL) {
		printf("Cipher %s not found\n", cipher_name);
		return ENOENT;
	}

	EVP_CIPHER_CTX_init(&ctx);
	EVP_EncryptInit(&ctx, evp, key, iv);
	EVP_EncryptUpdate(&ctx, out, &outl, in, in_len);
	EVP_EncryptFinal(&ctx, out + outl, &tmplen);

	return 0;
}

int
tc_decrypt(const char *cipher_name, unsigned char *key, unsigned char *iv,
    unsigned char *in, int in_len, unsigned char *out)
{
	const EVP_CIPHER *evp;
	EVP_CIPHER_CTX ctx;
	int outl, tmplen;

	evp = EVP_get_cipherbyname(cipher_name);
	if (evp == NULL) {
		printf("Cipher %s not found\n", cipher_name);
		return ENOENT;
	}

	EVP_CIPHER_CTX_init(&ctx);
	EVP_DecryptInit(&ctx, evp, key, iv);
	EVP_DecryptUpdate(&ctx, out, &outl, in, in_len);
	EVP_DecryptFinal(&ctx, out + outl, &tmplen);

	return 0;
}

int
pbkdf2(const char *pass, int passlen, const unsigned char *salt, int saltlen,
    int iter, const char *hash_name, int keylen, unsigned char *out)
{
	const EVP_MD *md;
	int r;

	md = EVP_get_digestbyname(hash_name);
	if (md == NULL) {
		printf("Hash %s not found\n", hash_name);
		return ENOENT;
	}
	r = PKCS5_PBKDF2_HMAC(pass, passlen, salt, saltlen, iter, md,
	    keylen, out);

	if (r == 0) {
		printf("Error in PBKDF2\n");
		return EINVAL;
	}

	return 0;
}

int
read_passphrase(char *pass, size_t passlen)
{
	struct termios termios_old, termios_new;
	ssize_t n;
	int fd, r = 0, cfd = 0;

	if ((fd = open("/dev/tty", O_RDONLY)) == -1) {
		fd = STDIN_FILENO;
		cfd = 1;
	}

	printf("Passphrase: ");
	fflush(stdout);

	memset(pass, 0, passlen);

	tcgetattr(fd, &termios_old);
	memcpy(&termios_new, &termios_old, sizeof(termios_new));
	termios_new.c_lflag &= ~ECHO;
	tcsetattr(fd, TCSAFLUSH, &termios_new);

	n = read(fd, pass, passlen-1);
	if (n > 0) {
		pass[n-1] = '\0'; /* Strip trailing \n */
	} else {
		r = EIO;
	}

	if (cfd)
		close(fd);

	tcsetattr(fd, TCSAFLUSH, &termios_old);
	putchar('\n');

	return r;
}

struct tchdr_dec *
decrypt_hdr(struct tchdr_enc *ehdr, char *algo, unsigned char *key)
{
	struct tchdr_dec *dhdr;
	unsigned char iv[128];
	int error;

	if ((dhdr = alloc_safe_mem(sizeof(struct tchdr_dec))) == NULL) {
		fprintf(stderr, "Error allocating safe tchdr_dec memory\n");
		return NULL;
	}

	memset(iv, 0, sizeof(iv));

	error = tc_decrypt(algo, key, iv, ehdr->enc, sizeof(struct tchdr_dec),
	    (unsigned char *)dhdr);
	if (error) {
		fprintf(stderr, "Header decryption failed\n");
		free_safe_mem(dhdr);
		return NULL;
	}

	BE_TO_HOST(16, dhdr->tc_ver);
	LE_TO_HOST(16, dhdr->tc_min_ver);
	BE_TO_HOST(32, dhdr->crc_keys);
	BE_TO_HOST(64, dhdr->vol_ctime);
	BE_TO_HOST(64, dhdr->hdr_ctime);
	BE_TO_HOST(64, dhdr->sz_hidvol);
	BE_TO_HOST(64, dhdr->sz_vol);
	BE_TO_HOST(64, dhdr->off_mk_scope);
	BE_TO_HOST(64, dhdr->sz_mk_scope);
	BE_TO_HOST(32, dhdr->flags);
	BE_TO_HOST(32, dhdr->sec_sz);
	BE_TO_HOST(32, dhdr->crc_dhdr);

	return dhdr;
}

int
verify_hdr(struct tchdr_dec *hdr)
{
	uint32_t crc;

	if (memcmp(hdr->tc_str, TC_SIG, sizeof(hdr->tc_str)) != 0) {
#ifdef DEBUG
		fprintf(stderr, "Signature mismatch\n");
#endif
		return 0;
	}

	crc = crc32((void *)&hdr->keys, 256);
	if (crc != hdr->crc_keys) {
#ifdef DEBUG
		fprintf(stderr, "CRC32 mismatch (crc_keys)\n");
#endif
		return 0;
	}

	switch(hdr->tc_ver) {
	case 1:
	case 2:
		/* Unsupported header version */
		fprintf(stderr, "Header version %d unsupported\n", hdr->tc_ver);
		return 0;

	case 3:
	case 4:
		hdr->sec_sz = 512;
		break;
	}

	return 1;
}

int
apply_keyfiles(unsigned char *pass, size_t pass_memsz, const char *keyfiles[],
    int nkeyfiles)
{
	int pl, k;
	unsigned char *kpool;
	unsigned char *kdata;
	int kpool_idx;
	size_t i, kdata_sz;
	uint32_t crc;

	if (pass_memsz < MAX_PASSSZ) {
		fprintf(stderr, "Not enough memory for password manipluation\n");
		return ENOMEM;
	}

	pl = strlen(pass);
	memset(pass+pl, 0, MAX_PASSSZ-pl);

	if ((kpool = alloc_safe_mem(KPOOL_SZ)) == NULL) {
		fprintf(stderr, "Error allocating memory for keyfile pool\n");
		return ENOMEM;
	}

	memset(kpool, 0, KPOOL_SZ);

	for (k = 0; k < nkeyfiles; k++) {
#ifdef DEBUG
		printf("Loading keyfile %s into kpool\n", keyfiles[k]);
#endif
		kpool_idx = 0;
		crc = ~0U;
		kdata_sz = MAX_KFILE_SZ;

		if ((kdata = read_to_safe_mem(keyfiles[k], 0, &kdata_sz)) == NULL) {
			fprintf(stderr, "Error reading keyfile %s content\n",
			    keyfiles[k]);
			free_safe_mem(kpool);
			return EIO;
		}

		for (i = 0; i < kdata_sz; i++) {
			crc = crc32_intermediate(crc, kdata[i]);

			kpool[kpool_idx++] += (unsigned char)(crc >> 24);
			kpool[kpool_idx++] += (unsigned char)(crc >> 16);
			kpool[kpool_idx++] += (unsigned char)(crc >> 8);
			kpool[kpool_idx++] += (unsigned char)(crc);

			/* Wrap around */
			if (kpool_idx == KPOOL_SZ)
				kpool_idx = 0;
		}

		free_safe_mem(kdata);
	}

#ifdef DEBUG
	printf("Applying kpool to passphrase\n");
#endif
	/* Apply keyfile pool to passphrase */
	for (i = 0; i < KPOOL_SZ; i++)
		pass[i] += kpool[i];

	free_safe_mem(kpool);

	return 0;
}

void
print_info(struct tcplay_info *info)
{
	printf("PBKDF2 PRF:\t\t%s\n", info->pbkdf_prf->name);
	printf("PBKDF2 iterations:\t%d\n", info->pbkdf_prf->iteration_count);
	printf("Cipher:\t\t\t%s\n", info->cipher->name);
	printf("Key Length:\t\t%d bits\n", info->cipher->klen*8);
	printf("CRC Key Data:\t\t%#x\n", info->hdr->crc_keys);
}

struct tcplay_info *
new_info(const char *dev, struct tc_crypto_algo *cipher,
    struct pbkdf_prf_algo *prf, struct tchdr_dec *hdr, off_t start)
{
	struct tcplay_info *info;
	size_t i;

	if ((info = (struct tcplay_info *)alloc_safe_mem(sizeof(*info))) == NULL) {
		fprintf(stderr, "could not allocate safe info memory");
		return NULL;
	}

	info->dev = dev;
	info->cipher = cipher;
	info->pbkdf_prf = prf;
	info->start = start;
	info->hdr = hdr;
	info->size = hdr->sz_mk_scope / hdr->sec_sz;	/* volume size */
	info->skip = hdr->off_mk_scope / hdr->sec_sz;	/* iv skip */
	info->offset = hdr->off_mk_scope / hdr->sec_sz;	/* block offset */

	for (i = 0; i < cipher->klen; i++) {
		sprintf(&info->key[i*2], "%02x", hdr->keys[i]);
	}

	return info;
}


int
process_hdr(const char *dev, unsigned char *pass, int passlen,
    struct tchdr_enc *ehdr, struct tcplay_info **pinfo)
{
	struct tchdr_dec *dhdr;
	struct tcplay_info *info;
	unsigned char *key;
	int i, j, found, error;

	*pinfo = NULL;

	if ((key = alloc_safe_mem(MAX_KEYSZ)) == NULL) {
		err(1, "could not allocate safe key memory");
	}

	/* Start search for correct algorithm combination */
	found = 0;
	for (i = 0; !found && pbkdf_prf_algos[i].name != NULL; i++) {
#ifdef DEBUG
		printf("\nTrying PRF algo %s (%d)\n", pbkdf_prf_algos[i].name,
		    pbkdf_prf_algos[i].iteration_count);
		printf("Salt: ");
		print_hex(ehdr->salt, 0, sizeof(ehdr->salt));
#endif
		error = pbkdf2(pass, passlen,
		    ehdr->salt, sizeof(ehdr->salt),
		    pbkdf_prf_algos[i].iteration_count,
		    pbkdf_prf_algos[i].name, MAX_KEYSZ, key);

		if (error)
			continue;

#if 0
		printf("Derived Key: ");
		print_hex(key, 0, MAX_KEYSZ);
#endif

		for (j = 0; !found && tc_crypto_algos[j].name != NULL; j++) {
#ifdef DEBUG
			printf("\nTrying cipher %s\n", tc_crypto_algos[j].name);
#endif

			dhdr = decrypt_hdr(ehdr, tc_crypto_algos[j].name, key);
			if (dhdr == NULL) {
				continue;
			}

			if (verify_hdr(dhdr)) {
#ifdef DEBUG
				printf("tc_str: %.4s, tc_ver: %zd, tc_min_ver: %zd, "
				    "crc_keys: %d, sz_vol: %"PRIu64", "
				    "off_mk_scope: %"PRIu64", sz_mk_scope: %"PRIu64", "
				    "flags: %d, sec_sz: %d crc_dhdr: %d\n",
				    dhdr->tc_str, dhdr->tc_ver, dhdr->tc_min_ver,
				    dhdr->crc_keys, dhdr->sz_vol, dhdr->off_mk_scope,
				    dhdr->sz_mk_scope, dhdr->flags, dhdr->sec_sz,
				    dhdr->crc_dhdr);
#endif
				found = 1;
			}
		}
	}

	free_safe_mem(key);

	if (!found)
		return EINVAL;

	if ((info = new_info(dev, &tc_crypto_algos[j-1], &pbkdf_prf_algos[i-1],
	    dhdr, 0)) == NULL) {
		return ENOMEM;
	}

	*pinfo = info;
	return 0;
}

int
dm_setup(const char *mapname, struct tcplay_info *info)
{
	struct dm_task *dmt = NULL;
	struct dm_info dmi;
	char *params = NULL;
	char *uu;
	uint32_t status;
	int ret = 0;

	if ((params = alloc_safe_mem(512)) == NULL) {
		fprintf(stderr, "could not allocate safe parameters memory");
		return ENOMEM;
		
	}

	/* aes-cbc-essiv:sha256 7997f8af... 0 /dev/ad0s0a 8 */
	/*			   iv off---^  block off--^ */
	snprintf(params, 512, "%s %s %"PRIu64 " %s %"PRIu64,
	    info->cipher->dm_crypt_str, info->key,
	    info->skip, info->dev, info->offset);
#ifdef DEBUG
	printf("Params: %s\n", params);
#endif
	if ((dmt = dm_task_create(DM_DEVICE_CREATE)) == NULL) {
		fprintf(stderr, "dm_task_create failed\n");
		ret = -1;
		goto out;
	}

	if ((dm_task_set_name(dmt, mapname)) == 0) {
		fprintf(stderr, "dm_task_set_name failed\n");
		ret = -1;
		goto out;
	}

	uuid_create(&info->uuid, &status);
	if (status != uuid_s_ok) {
		fprintf(stderr, "uuid_create failed\n");
		ret = -1;
		goto out;
	}

	uuid_to_string(&info->uuid, &uu, &status);
	if (uu == NULL) {
		fprintf(stderr, "uuid_to_string failed\n");
		ret = -1;
		goto out;
	}

	if ((dm_task_set_uuid(dmt, uu)) == 0) {
		free(uu);
		fprintf(stderr, "dm_task_set_uuid failed\n");
		ret = -1;
		goto out;
	}
	free(uu);

	if ((dm_task_add_target(dmt, info->start, info->size, "crypt", params)) == 0) {
		fprintf(stderr, "dm_task_add_target failed\n");
		ret = -1;
		goto out;
	}

	if ((dm_task_run(dmt)) == 0) {
		fprintf(stderr, "dm_task_task_run failed\n");
		ret = -1;
		goto out;
	}

	if ((dm_task_get_info(dmt, &dmi)) == 0) {
		fprintf(stderr, "dm_task_get info failed\n");
		/* XXX: probably do more than just erroring out... */
		ret = -1;
		goto out;
	}

out:
	free_safe_mem(params);
	if (dmt)
		dm_task_destroy(dmt);

	return ret;
}

static void
usage(void)
{
	fprintf(stderr,
	    "Usage: tc-play <command> [options]\n"
	    "Valid commands and its arguments are:\n"
	    " -i\n"
	    "\t Gives information about the TC volume specified by -d\n"
	    " -m <mapping name>\n"
	    "\t Creates a dm-crypt mapping for the device specified by -d\n"
	    "Valid options and its arguments are:\n"
	    " -d <device path>\n"
	    "\t specifies the path to the volume to operate on (e.g. /dev/da0s1)\n"
	    " -s <disk path>\n"
	    "\t specifies that the disk (e.g. /dev/da0) is using system encryption\n"
	    " -e\n"
	    " protect a hidden volume when mounting the outer volume\n"
	    );

	exit(1);
}

int
main(int argc, char *argv[])
{
	const char *dev = NULL, *sys_dev = NULL, *map_name = NULL;
	const char *keyfiles[MAX_KEYFILES];
	char *pass;
	struct tchdr_enc *ehdr, *hehdr;
	struct tcplay_info *info;
	int nkeyfiles;
	int ch, error, r = 0;
	int sflag = 0, iflag = 0, mflag = 0, hflag = 0;
	size_t sz;

	OpenSSL_add_all_algorithms();

	nkeyfiles = 0;

	while ((ch = getopt(argc, argv, "d:eik:m:s:v")) != -1) {
		switch(ch) {
		case 'd':
			dev = optarg;
			break;
		case 'e':
			hflag = 1;
			break;
		case 'i':
			iflag = 1;
			break;
		case 'k':
			keyfiles[nkeyfiles++] = optarg;
			break;
		case 'm':
			mflag = 1;
			map_name = optarg;
			break;
		case 's':
			sflag = 1;
			sys_dev = optarg;
			break;
		case 'v':
			printf("tc-play v%d.%d\n", MAJ_VER, MIN_VER);
			exit(0);
			/* NOT REACHED */
		case 'h':
		case '?':
		default:
			usage();
			/* NOT REACHED */
		}
	}

	argc -= optind;
	argv += optind;

	/* Check arguments */
	if (!((mflag || iflag) && dev != NULL) ||
	    (mflag && iflag) ||
	    (sflag && (sys_dev == NULL)) ||
	    (mflag && (map_name == NULL))) {
		usage();
		/* NOT REACHED */
	}

	if ((pass = alloc_safe_mem(MAX_PASSSZ)) == NULL) {
		err(1, "could not allocate safe passphrase memory");
	}

	if ((error = read_passphrase(pass, MAX_PASSSZ))) {
		err(1, "could not read passphrase");
	}

	if (nkeyfiles > 0) {
		/* Apply keyfiles to 'pass' */
		if ((error = apply_keyfiles(pass, MAX_PASSSZ, keyfiles,
		    nkeyfiles))) {
			err(1, "could not apply keyfiles");
		}
	}

	sz = HDRSZ;
	ehdr = (struct tchdr_enc *)read_to_safe_mem((sflag) ? sys_dev : dev,
	    (sflag) ? HDR_OFFSET_SYS : 0, &sz);
	if (ehdr == NULL) {
		err(1, "read hdr_enc: %s", dev);
	}

	if (!sflag) {
		sz = HDRSZ;
		hehdr = (struct tchdr_enc *)read_to_safe_mem(dev, HDR_OFFSET_HIDDEN, &sz);
		if (hehdr == NULL) {
			err(1, "read hdr_enc: %s", dev);
		}
	} else {
		hehdr = NULL;
	}

#if 1
	if ((error = process_hdr(dev, pass, (nkeyfiles > 0)?MAX_PASSSZ:strlen(pass),
	    ehdr, &info)) != 0) {
		if (hehdr) {
			if ((error = process_hdr(dev, pass, (nkeyfiles > 0)?MAX_PASSSZ:strlen(pass),
			hehdr, &info)) != 0) {
				free_safe_mem(hehdr);
				r = 1;
				fprintf(stderr, "Incorrect password or not a TrueCrypt volume\n");
				goto out;
			}
		} else {
			r = 1;
			fprintf(stderr, "Incorrect password or not a TrueCrypt volume\n");
			goto out;
		}
	}
#endif

	if (iflag) {
		print_info(info);
	} else if (mflag) {
		if ((error = dm_setup(map_name, info)) != 0) {
			err(1, "could not set up dm-crypt mapping");
		}
		printf("All ok!");
	}

out:
	if (hehdr)
		free_safe_mem(hehdr);
	free_safe_mem(ehdr);
	free_safe_mem(pass);
	if (info)
		free_safe_mem(info);

	return r;
}
