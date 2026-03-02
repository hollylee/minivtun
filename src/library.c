/*
 * Copyright (c) 2015 Justin Liu
 * Author: Justin Liu <rssnsj@gmail.com>
 * https://github.com/rssnsj/minivtun
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <assert.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <openssl/evp.h>
#include <openssl/md5.h>
#include <openssl/rand.h>
#include <sys/types.h>
#include <netdb.h>

#include "library.h"
#include "minivtun.h"

struct name_cipher_pair cipher_pairs[] = {
	{ "aes-128", EVP_aes_128_cbc, },
	{ "aes-256", EVP_aes_256_cbc, },
	{ "des", EVP_des_cbc, },
	{ "desx", EVP_desx_cbc, },
	{ "rc4", EVP_rc4, },
	{ NULL, NULL, },
};

const void *get_crypto_type(const char *name)
{
	const EVP_CIPHER *cipher = NULL;
	int i;

	for (i = 0; cipher_pairs[i].name; i++) {
		if (strcasecmp(cipher_pairs[i].name, name) == 0) {
			cipher = ((const EVP_CIPHER *(*)(void))cipher_pairs[i].cipher)();
			break;
		}
	}

	if (cipher) {
		assert(EVP_CIPHER_key_length(cipher) <= CRYPTO_MAX_KEY_SIZE);
		assert(EVP_CIPHER_iv_length(cipher) <= CRYPTO_MAX_BLOCK_SIZE);
		return cipher;
	} else {
		return NULL;
	}
}

/* Hardcoded IV used when --random-iv is not specified (legacy wire format). */
static const unsigned char crypto_ivec_initdata[CRYPTO_MAX_BLOCK_SIZE] = {
	0xab, 0xcd, 0xef, 0x12, 0x34, 0x56, 0x78, 0x90,
	0xab, 0xcd, 0xef, 0x12, 0x34, 0x56, 0x78, 0x90,
	0xab, 0xcd, 0xef, 0x12, 0x34, 0x56, 0x78, 0x90,
	0xab, 0xcd, 0xef, 0x12, 0x34, 0x56, 0x78, 0x90,
};

void datagram_encrypt(const void *key, const void *cptype, void *in,
		void *out, size_t *dlen)
{
	size_t iv_len = EVP_CIPHER_iv_length((const EVP_CIPHER *)cptype);
	EVP_CIPHER_CTX *ctx;
	unsigned char iv[CRYPTO_MAX_BLOCK_SIZE];
	int outl = 0, outl2 = 0;
	size_t orig_len = *dlen;
	size_t last, padded_len;
	uint8_t *outbuf = (uint8_t *)out;
	size_t data_offset;

	if (iv_len == 0)
		iv_len = 16;

	if (config.random_iv) {
		/* Generate a fresh random IV for every message and prepend it. */
		if (RAND_bytes(iv, (int)iv_len) != 1)
			memset(iv, 0, iv_len);
		memcpy(outbuf, iv, iv_len);
		data_offset = iv_len;
	} else {
		/* Legacy mode: fixed IV, no IV prefix in the wire packet. */
		memcpy(iv, crypto_ivec_initdata, iv_len);
		data_offset = 0;
	}

	/* Zero-pad plaintext to next block boundary, then encrypt in-place. */
	last = orig_len % iv_len;
	padded_len = last ? orig_len + (iv_len - last) : orig_len;
	memcpy(outbuf + data_offset, in, orig_len);
	if (padded_len > orig_len)
		memset(outbuf + data_offset + orig_len, 0, padded_len - orig_len);

	ctx = EVP_CIPHER_CTX_new();
	if (!ctx) { *dlen = 0; return; }
	if (!EVP_EncryptInit_ex(ctx, cptype, NULL, key, iv))
		{ EVP_CIPHER_CTX_free(ctx); *dlen = 0; return; }
	EVP_CIPHER_CTX_set_padding(ctx, 0);
	if (!EVP_EncryptUpdate(ctx, outbuf + data_offset, &outl,
	                       outbuf + data_offset, (int)padded_len))
		{ EVP_CIPHER_CTX_free(ctx); *dlen = 0; return; }
	if (!EVP_EncryptFinal_ex(ctx, outbuf + data_offset + outl, &outl2))
		{ EVP_CIPHER_CTX_free(ctx); *dlen = 0; return; }
	EVP_CIPHER_CTX_free(ctx);

	*dlen = data_offset + (size_t)(outl + outl2);
}

void datagram_decrypt(const void *key, const void *cptype, void *in,
		void *out, size_t *dlen)
{
	size_t iv_len = EVP_CIPHER_iv_length((const EVP_CIPHER *)cptype);
	EVP_CIPHER_CTX *ctx;
	unsigned char iv[CRYPTO_MAX_BLOCK_SIZE];
	int outl = 0, outl2 = 0;
	uint8_t *inbuf = (uint8_t *)in;

	if (iv_len == 0)
		iv_len = 16;

	if (config.random_iv) {
		/* Extract the per-message IV from the front of the packet. */
		if (*dlen <= iv_len) { *dlen = 0; return; }
		memcpy(iv, inbuf, iv_len);
		inbuf  += iv_len;
		*dlen  -= iv_len;
	} else {
		/* Legacy mode: fixed IV, ciphertext starts at byte 0. */
		memcpy(iv, crypto_ivec_initdata, iv_len);
	}

	ctx = EVP_CIPHER_CTX_new();
	if (!ctx) { *dlen = 0; return; }
	if (!EVP_DecryptInit_ex(ctx, cptype, NULL, key, iv))
		{ EVP_CIPHER_CTX_free(ctx); *dlen = 0; return; }
	EVP_CIPHER_CTX_set_padding(ctx, 0);
	if (!EVP_DecryptUpdate(ctx, out, &outl, inbuf, (int)*dlen))
		{ EVP_CIPHER_CTX_free(ctx); *dlen = 0; return; }
	if (!EVP_DecryptFinal_ex(ctx, (unsigned char *)out + outl, &outl2))
		{ EVP_CIPHER_CTX_free(ctx); *dlen = 0; return; }
	EVP_CIPHER_CTX_free(ctx);

	*dlen = (size_t)(outl + outl2);
}

void fill_with_string_md5sum(const char *in, void *out, size_t outlen)
{
	char *outp = out, *oute = outp + outlen;
	unsigned int realOutLen = 0;
	/*
	MD5_CTX ctx;

	MD5_Init(&ctx);
	MD5_Update(&ctx, in, strlen(in));
	MD5_Final(out, &ctx);
	*/
	EVP_MD_CTX * ctx = EVP_MD_CTX_new();
	EVP_DigestInit_ex(ctx, EVP_md5(), NULL);
	EVP_DigestUpdate(ctx, in, strlen(in));
	EVP_DigestFinal(ctx, out, &realOutLen);
	EVP_MD_CTX_free(ctx);

	/* Fill in remaining buffer with repeated data. */
	for (outp += 16; outp < oute; outp += 16) {
		size_t bs = (oute - outp >= 16) ? 16 : (oute - outp);
		memcpy(outp, out, bs);
	}
}

/* =-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-= */

int get_sockaddr_inx_pair(const char *pair, struct sockaddr_inx *sa)
{
	struct addrinfo hints, *result;
	char host[51] = "", s_port[10] = "";
	int port = 0, rc;

	/* Only getting an INADDR_ANY address. */
	if (pair == NULL) {
		struct sockaddr_in *sa4 = (struct sockaddr_in *)sa;
		sa4->sin_family = AF_INET;
		sa4->sin_addr.s_addr = 0;
		sa4->sin_port = 0;
		return 0;
	}

	if (sscanf(pair, "[%50[^]]]:%d", host, &port) == 2) {
	} else if (sscanf(pair, "%50[^:]:%d", host, &port) == 2) {
	} else {
		/**
		 * Address with a single port number, usually for
		 * local IPv4 listen address.
		 * e.g., "10000" is considered as "0.0.0.0:10000"
		 */
		const char *sp;
		for (sp = pair; *sp; sp++) {
			if (!(*sp >= '0' && *sp <= '9'))
				return -EINVAL;
		}
		sscanf(pair, "%d", &port);
		strcpy(host, "0.0.0.0");
	}
	if (port <= 0 || port > 65535)
		return -EINVAL;
	snprintf(s_port, sizeof(s_port), "%d", port);

	memset(&hints, 0, sizeof(struct addrinfo));
	hints.ai_family = AF_UNSPEC;  /* Allow IPv4 or IPv6 */
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;  /* For wildcard IP address */
	hints.ai_protocol = 0;        /* Any protocol */
	hints.ai_canonname = NULL;
	hints.ai_addr = NULL;
	hints.ai_next = NULL;

	if ((rc = getaddrinfo(host, s_port, &hints, &result)))
		return -EAGAIN;

	/* Get the first resolution. */
	memcpy(sa, result->ai_addr, result->ai_addrlen);

	freeaddrinfo(result);
	return 0;
}

void do_daemonize(void)
{
	pid_t pid;
	int fd;

	/* Fork off the parent process */
	if ((pid = fork()) < 0) {
		/* Error */
		fprintf(stderr, "*** fork() error: %s.\n", strerror(errno));
		exit(1);
	} else if (pid > 0) {
		/* Let the parent process terminate */
		exit(0);
	}

	/* Do this before child process quits to prevent duplicate printf output */
	if ((fd = open("/dev/null", O_RDWR)) >= 0) {
		dup2(fd, 0);
		dup2(fd, 1);
		dup2(fd, 2);
		if (fd > 2)
			close(fd);
	}

	/* Catch, ignore and handle signals */
	signal(SIGCHLD, SIG_IGN);
	signal(SIGHUP, SIG_IGN);

	/* Let the child process become session leader */
	if (setsid() < 0)
		exit(1);

	if ((pid = fork()) < 0) {
		/* Error */
		exit(1);
	} else if (pid > 0) {
		/* Let the parent process terminate */
		exit(0);
	}

	/* OK, set up the grandchild process */
	chdir("/tmp");
}

