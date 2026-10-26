/*
 * sweetstored: Root helper daemon for SweetStore.app (127.0.0.1:29170).
 *
 * Unpacks decrypted .ipa archives (streaming ZIP32/ZIP64 via zlib), thins
 * universal binaries to arm64, verifies FairPlay cryptid == 0, embeds ad-hoc
 * SHA-256 LC_CODE_SIGNATURE + XML entitlements when missing, installs into
 * /Applications/<App>.app (root:wheel), and rebuilds LaunchServices via
 * /var/local/bin/lsreg -r. Also ensures SweetStore.app itself is registered
 * on first boot.
 */
#import <Foundation/Foundation.h>
#include <CommonCrypto/CommonDigest.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <libkern/OSByteOrder.h>
#include <mach-o/fat.h>
#include <mach-o/loader.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <zlib.h>

extern char **environ;

#define SWEETSTORED_PORT        29170
#define STAGE_ROOT              "/private/var/local/store-stage"
#define LOCAL_IPA_DIR           "/private/var/local/ipa"
#define REG_STAMP_PATH          "/private/var/local/.sweetstore-registered-v1"
#define MARKER_FILE             ".sweetstore_installed"

static pthread_mutex_t g_job_lock = PTHREAD_MUTEX_INITIALIZER;
static BOOL g_busy = NO;
static NSString *g_state = @"idle";
static double g_progress = 0.0;
static NSString *g_step = @"Ready";
static NSString *g_app_name = @"";
static NSString *g_bundle_id = @"";
static NSString *g_error = @"";

static void set_job(NSString *state, double progress, NSString *step, NSString *appName, NSString *bundleId, NSString *err)
{
	pthread_mutex_lock(&g_job_lock);
	if (state) g_state = [state copy];
	if (progress >= 0.0) g_progress = progress;
	if (step) g_step = [step copy];
	if (appName) g_app_name = [appName copy];
	if (bundleId) g_bundle_id = [bundleId copy];
	if (err) g_error = [err copy];
	pthread_mutex_unlock(&g_job_lock);
}

static int run_tool(const char *prog, char *const argv[])
{
	pid_t pid = 0;
	int status = 0;
	int rc = posix_spawn(&pid, prog, NULL, NULL, argv, environ);
	if (rc != 0) {
		fprintf(stderr, "sweetstored: posix_spawn %s failed: %d\n", prog, rc);
		return -1;
	}
	while (waitpid(pid, &status, 0) < 0) {
		if (errno != EINTR) return -1;
	}
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int run_lsreg_rebuild(void)
{
	char *const argv[] = { (char *)"/var/local/bin/lsreg", (char *)"-r", NULL };
	return run_tool("/var/local/bin/lsreg", argv);
}

static void chown_chmod_recursive(NSString *rootPath)
{
	NSFileManager *fm = [NSFileManager defaultManager];
	NSDirectoryEnumerator *en = [fm enumeratorAtPath:rootPath];
	chown(rootPath.fileSystemRepresentation, 0, 0);
	chmod(rootPath.fileSystemRepresentation, 0755);
	NSString *rel;
	while ((rel = [en nextObject]) != nil) {
		NSString *full = [rootPath stringByAppendingPathComponent:rel];
		const char *cpath = full.fileSystemRepresentation;
		struct stat st;
		if (lstat(cpath, &st) != 0) continue;
		lchown(cpath, 0, 0);
		if (S_ISLNK(st.st_mode)) continue;
		if (S_ISDIR(st.st_mode)) {
			chmod(cpath, 0755);
		}
	}
}

/* ---- Streaming ZIP32 / ZIP64 extractor via zlib ---- */

static uint16_t rd16le(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return OSSwapLittleToHostInt16(v); }
static uint32_t rd32le(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return OSSwapLittleToHostInt32(v); }
static uint64_t rd64le(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return OSSwapLittleToHostInt64(v); }

static BOOL pread_all(int fd, void *buf, size_t len, off_t off)
{
	uint8_t *p = (uint8_t *)buf;
	size_t done = 0;
	while (done < len) {
		ssize_t n = pread(fd, p + done, len - done, off + (off_t)done);
		if (n <= 0) return NO;
		done += (size_t)n;
	}
	return YES;
}

static BOOL write_all(int fd, const void *buf, size_t len)
{
	const uint8_t *p = (const uint8_t *)buf;
	size_t done = 0;
	while (done < len) {
		ssize_t n = write(fd, p + done, len - done);
		if (n <= 0) {
			if (n < 0 && errno == EINTR) continue;
			return NO;
		}
		done += (size_t)n;
	}
	return YES;
}

static BOOL safe_rel_path(NSString *rel)
{
	if (rel.length == 0 || [rel hasPrefix:@"/"] || [rel containsString:@".."]) {
		for (NSString *comp in [rel pathComponents]) {
			if ([comp isEqualToString:@".."] || [comp isEqualToString:@"/"]) return NO;
		}
		if ([rel hasPrefix:@"/"]) return NO;
	}
	return rel.length > 0;
}

static BOOL extract_ipa_zip(NSString *ipaPath, NSString *destDir, double pStart, double pEnd, NSString **errOut)
{
	int fd = open(ipaPath.fileSystemRepresentation, O_RDONLY);
	if (fd < 0) {
		if (errOut) *errOut = [NSString stringWithFormat:@"Cannot open IPA (%s)", strerror(errno)];
		return NO;
	}
	struct stat st;
	if (fstat(fd, &st) != 0 || st.st_size < 22) {
		close(fd);
		if (errOut) *errOut = @"Invalid or empty .ipa file";
		return NO;
	}
	uint64_t fileSize = (uint64_t)st.st_size;
	size_t tailLen = (size_t)MIN((uint64_t)65558, fileSize);
	uint8_t *tail = (uint8_t *)malloc(tailLen);
	if (!tail || !pread_all(fd, tail, tailLen, (off_t)(fileSize - tailLen))) {
		free(tail);
		close(fd);
		if (errOut) *errOut = @"Failed reading ZIP directory";
		return NO;
	}

	ssize_t eocdPos = -1;
	for (ssize_t i = (ssize_t)tailLen - 22; i >= 0; i--) {
		if (rd32le(tail + i) == 0x06054b50u) {
			eocdPos = i;
			break;
		}
	}
	if (eocdPos < 0) {
		free(tail);
		close(fd);
		if (errOut) *errOut = @"Not a valid ZIP/.ipa archive (missing EOCD)";
		return NO;
	}

	uint64_t cdCount = rd16le(tail + eocdPos + 10);
	uint64_t cdSize  = rd32le(tail + eocdPos + 12);
	uint64_t cdOff   = rd32le(tail + eocdPos + 16);

	/* Check ZIP64 EOCD locator (20 bytes immediately before EOCD) */
	if (eocdPos >= 20 && rd32le(tail + eocdPos - 20) == 0x07064b50u) {
		uint64_t z64Off = rd64le(tail + eocdPos - 12);
		uint8_t z64[56];
		if (z64Off + 56 <= fileSize && pread_all(fd, z64, sizeof(z64), (off_t)z64Off) &&
		    rd32le(z64) == 0x06064b50u) {
			cdCount = rd64le(z64 + 32);
			cdSize  = rd64le(z64 + 40);
			cdOff   = rd64le(z64 + 48);
		}
	}
	free(tail);

	if (cdOff + cdSize > fileSize || cdSize > (128u << 20)) {
		close(fd);
		if (errOut) *errOut = @"Corrupt ZIP central directory bounds";
		return NO;
	}

	uint8_t *cd = (uint8_t *)malloc((size_t)cdSize);
	if (!cd || !pread_all(fd, cd, (size_t)cdSize, (off_t)cdOff)) {
		free(cd);
		close(fd);
		if (errOut) *errOut = @"Failed reading ZIP central directory";
		return NO;
	}

	NSFileManager *fm = [NSFileManager defaultManager];
	uint8_t *inBuf = (uint8_t *)malloc(65536);
	uint8_t *outBuf = (uint8_t *)malloc(65536);
	size_t pos = 0;
	BOOL ok = YES;

	for (uint64_t idx = 0; idx < cdCount && pos + 46 <= (size_t)cdSize; idx++) {
		if (rd32le(cd + pos) != 0x02014b50u) {
			ok = NO;
			if (errOut) *errOut = @"Corrupt central directory entry";
			break;
		}
		uint16_t method  = rd16le(cd + pos + 10);
		uint64_t compSz  = rd32le(cd + pos + 20);
		uint64_t uncompSz = rd32le(cd + pos + 24);
		uint16_t nameLen = rd16le(cd + pos + 28);
		uint16_t extraLen = rd16le(cd + pos + 30);
		uint16_t commLen = rd16le(cd + pos + 32);
		uint32_t extAttr = rd32le(cd + pos + 38);
		uint64_t lfhOff  = rd32le(cd + pos + 42);

		if (pos + 46 + nameLen + extraLen + commLen > (size_t)cdSize) {
			ok = NO;
			if (errOut) *errOut = @"Truncated central directory entry";
			break;
		}

		/* Parse ZIP64 extra field (0x0001) if needed */
		const uint8_t *extra = cd + pos + 46 + nameLen;
		size_t ep = 0;
		while (ep + 4 <= extraLen) {
			uint16_t tag = rd16le(extra + ep);
			uint16_t elen = rd16le(extra + ep + 2);
			if (ep + 4 + elen > extraLen) break;
			if (tag == 0x0001) {
				size_t zp = ep + 4;
				if (uncompSz == 0xffffffffu && zp + 8 <= ep + 4 + elen) { uncompSz = rd64le(extra + zp); zp += 8; }
				if (compSz == 0xffffffffu && zp + 8 <= ep + 4 + elen)   { compSz   = rd64le(extra + zp); zp += 8; }
				if (lfhOff == 0xffffffffu && zp + 8 <= ep + 4 + elen)   { lfhOff   = rd64le(extra + zp); zp += 8; }
			}
			ep += 4 + elen;
		}

		NSString *entryName = [[NSString alloc] initWithBytes:(cd + pos + 46) length:nameLen encoding:NSUTF8StringEncoding];
		if (!entryName) {
			entryName = [[NSString alloc] initWithBytes:(cd + pos + 46) length:nameLen encoding:NSISOLatin1StringEncoding];
		}
		pos += 46 + nameLen + extraLen + commLen;

		if (!entryName || ![entryName hasPrefix:@"Payload/"] || !safe_rel_path(entryName)) {
			continue;
		}

		if ((idx & 31) == 0 && cdCount > 0) {
			double frac = pStart + (pEnd - pStart) * ((double)idx / (double)cdCount);
			set_job(@"extracting", frac, [NSString stringWithFormat:@"Extracting (%llu/%llu)...",
			    (unsigned long long)idx, (unsigned long long)cdCount], nil, nil, nil);
		}

		NSString *outPath = [destDir stringByAppendingPathComponent:entryName];
		uint16_t unixMode = (uint16_t)(extAttr >> 16);
		BOOL isDir = [entryName hasSuffix:@"/"] || ((unixMode & S_IFMT) == S_IFDIR);
		BOOL isSymlink = ((unixMode & S_IFMT) == S_IFLNK);

		if (isDir) {
			[fm createDirectoryAtPath:outPath withIntermediateDirectories:YES attributes:nil error:nil];
			continue;
		}

		[fm createDirectoryAtPath:[outPath stringByDeletingLastPathComponent]
		    withIntermediateDirectories:YES attributes:nil error:nil];

		uint8_t lfh[30];
		if (lfhOff + 30 > fileSize || !pread_all(fd, lfh, 30, (off_t)lfhOff) || rd32le(lfh) != 0x04034b50u) {
			ok = NO;
			if (errOut) *errOut = @"Invalid local file header in IPA";
			break;
		}
		uint64_t dataOff = lfhOff + 30 + rd16le(lfh + 26) + rd16le(lfh + 28);
		if (dataOff + compSz > fileSize) {
			ok = NO;
			if (errOut) *errOut = @"Compressed entry exceeds archive size";
			break;
		}

		if (isSymlink) {
			if (uncompSz > 4096) continue;
			NSMutableData *symData = [NSMutableData dataWithCapacity:(NSUInteger)uncompSz];
			if (method == 0) {
				symData.length = (NSUInteger)compSz;
				if (!pread_all(fd, symData.mutableBytes, (size_t)compSz, (off_t)dataOff)) { ok = NO; break; }
			} else if (method == 8) {
				z_stream zs; memset(&zs, 0, sizeof(zs));
				if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) { ok = NO; break; }
				uint64_t rem = compSz, cur = dataOff;
				while (rem > 0) {
					size_t chunk = (size_t)MIN(rem, (uint64_t)65536);
					if (!pread_all(fd, inBuf, chunk, (off_t)cur)) { ok = NO; break; }
					cur += chunk; rem -= chunk;
					zs.next_in = inBuf; zs.avail_in = (uInt)chunk;
					do {
						zs.next_out = outBuf; zs.avail_out = 65536;
						int zrc = inflate(&zs, rem == 0 ? Z_FINISH : Z_NO_FLUSH);
						if (zrc != Z_OK && zrc != Z_STREAM_END && zrc != Z_BUF_ERROR) { ok = NO; break; }
						size_t got = 65536 - zs.avail_out;
						if (got > 0) [symData appendBytes:outBuf length:got];
					} while (zs.avail_out == 0 && ok);
					if (!ok) break;
				}
				inflateEnd(&zs);
				if (!ok) break;
			} else {
				continue;
			}
			NSString *target = [[NSString alloc] initWithData:symData encoding:NSUTF8StringEncoding];
			if (target.length > 0 && ![target hasPrefix:@"/"]) {
				unlink(outPath.fileSystemRepresentation);
				symlink(target.fileSystemRepresentation, outPath.fileSystemRepresentation);
			}
			continue;
		}

		unlink(outPath.fileSystemRepresentation);
		int outFd = open(outPath.fileSystemRepresentation, O_CREAT | O_TRUNC | O_WRONLY,
		    (unixMode & 0111) ? 0755 : 0644);
		if (outFd < 0) {
			ok = NO;
			if (errOut) *errOut = [NSString stringWithFormat:@"Cannot create file %@", entryName];
			break;
		}

		if (method == 0) {
			uint64_t rem = compSz, cur = dataOff;
			while (rem > 0) {
				size_t chunk = (size_t)MIN(rem, (uint64_t)65536);
				if (!pread_all(fd, inBuf, chunk, (off_t)cur) || !write_all(outFd, inBuf, chunk)) {
					ok = NO; break;
				}
				cur += chunk; rem -= chunk;
			}
		} else if (method == 8) {
			z_stream zs;
			memset(&zs, 0, sizeof(zs));
			if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) {
				close(outFd); ok = NO; break;
			}
			uint64_t rem = compSz, cur = dataOff;
			while (rem > 0) {
				size_t chunk = (size_t)MIN(rem, (uint64_t)65536);
				if (!pread_all(fd, inBuf, chunk, (off_t)cur)) { ok = NO; break; }
				cur += chunk; rem -= chunk;
				zs.next_in = inBuf;
				zs.avail_in = (uInt)chunk;
				do {
					zs.next_out = outBuf;
					zs.avail_out = 65536;
					int zrc = inflate(&zs, rem == 0 ? Z_FINISH : Z_NO_FLUSH);
					if (zrc != Z_OK && zrc != Z_STREAM_END && zrc != Z_BUF_ERROR) {
						ok = NO;
						if (errOut) *errOut = [NSString stringWithFormat:@"Decompression error (%d) in %@", zrc, entryName];
						break;
					}
					size_t got = 65536 - zs.avail_out;
					if (got > 0 && !write_all(outFd, outBuf, got)) { ok = NO; break; }
				} while (zs.avail_out == 0 && ok);
				if (!ok) break;
			}
			inflateEnd(&zs);
		} else {
			if (errOut) *errOut = [NSString stringWithFormat:@"Unsupported ZIP compression method %u in %@", method, entryName];
			ok = NO;
		}
		close(outFd);
		if (!ok) break;
	}

	free(inBuf);
	free(outBuf);
	free(cd);
	close(fd);
	return ok;
}

/* ---- Mach-O thinning, FairPlay cryptid check & ad-hoc SHA-256 signing ---- */

static NSData *build_adhoc_superblob(int fd, uint64_t codeLimit, uint64_t execSegBase, uint64_t execSegLimit,
                                     BOOL isMainBinary, NSString *identStr, NSData *xmlEnts)
{
	const uint32_t pageShift = 12;
	const uint32_t pageSize = 1u << pageShift;
	uint32_t nCodeSlots = (uint32_t)((codeLimit + pageSize - 1) / pageSize);
	uint32_t nSpecialSlots = xmlEnts.length > 0 ? 5 : 0;

	const char *identC = (identStr.length > 0 ? identStr.UTF8String : "org.iosonpixel.app");
	size_t identLen = strlen(identC) + 1;

	NSMutableData *entBlob = nil;
	uint8_t entHash[CC_SHA256_DIGEST_LENGTH];
	if (nSpecialSlots > 0) {
		uint32_t magic = OSSwapHostToBigInt32(0xfade7171u);
		uint32_t len   = OSSwapHostToBigInt32((uint32_t)(8 + xmlEnts.length));
		entBlob = [NSMutableData dataWithCapacity:8 + xmlEnts.length];
		[entBlob appendBytes:&magic length:4];
		[entBlob appendBytes:&len length:4];
		[entBlob appendData:xmlEnts];
		CC_SHA256(entBlob.bytes, (CC_LONG)entBlob.length, entHash);
	}

	uint32_t hdrLen = 88;
	uint32_t identOff = hdrLen;
	uint32_t hashOff = identOff + (uint32_t)identLen + nSpecialSlots * 32;
	uint32_t cdLen = hashOff + nCodeSlots * 32;

	NSMutableData *cd = [NSMutableData dataWithCapacity:cdLen];
	uint32_t w32[12];
	w32[0] = OSSwapHostToBigInt32(0xfade0c02u);                 /* CSMAGIC_CODEDIRECTORY */
	w32[1] = OSSwapHostToBigInt32(cdLen);
	w32[2] = OSSwapHostToBigInt32(0x00020400u);                 /* version 2.4.0 */
	w32[3] = OSSwapHostToBigInt32(0x00000002u);                 /* CS_ADHOC */
	w32[4] = OSSwapHostToBigInt32(hashOff);
	w32[5] = OSSwapHostToBigInt32(identOff);
	w32[6] = OSSwapHostToBigInt32(nSpecialSlots);
	w32[7] = OSSwapHostToBigInt32(nCodeSlots);
	w32[8] = OSSwapHostToBigInt32(codeLimit <= 0xffffffffULL ? (uint32_t)codeLimit : 0xffffffffu);
	[cd appendBytes:w32 length:9 * 4];

	uint8_t b4[4] = { 32, 2, 0, (uint8_t)pageShift };           /* SHA-256, 4K pages */
	[cd appendBytes:b4 length:4];
	uint32_t zeros4[4] = { 0, 0, 0, 0 };                        /* spare2, scatter, team, spare3 */
	[cd appendBytes:zeros4 length:16];

	uint64_t q64[4];
	q64[0] = OSSwapHostToBigInt64(codeLimit > 0xffffffffULL ? codeLimit : 0);
	q64[1] = OSSwapHostToBigInt64(execSegBase);
	q64[2] = OSSwapHostToBigInt64(execSegLimit);
	q64[3] = OSSwapHostToBigInt64(isMainBinary ? 1ULL : 0ULL);  /* CS_EXECSEG_MAIN_BINARY */
	[cd appendBytes:q64 length:32];

	[cd appendBytes:identC length:identLen];
	if (nSpecialSlots > 0) {
		[cd appendBytes:entHash length:32];                 /* slot -5: entitlements */
		uint8_t z4[128] = { 0 };                            /* slots -4..-1 */
		[cd appendBytes:z4 length:128];
	}

	uint8_t pageBuf[4096];
	for (uint64_t off = 0; off < codeLimit; off += pageSize) {
		size_t chunk = (size_t)MIN((uint64_t)pageSize, codeLimit - off);
		if (!pread_all(fd, pageBuf, chunk, (off_t)off)) return nil;
		uint8_t digest[CC_SHA256_DIGEST_LENGTH];
		CC_SHA256(pageBuf, (CC_LONG)chunk, digest);
		[cd appendBytes:digest length:32];
	}

	uint32_t blobCount = entBlob ? 2 : 1;
	uint32_t sbHdr = 12 + blobCount * 8;
	uint32_t sbLen = sbHdr + (uint32_t)cd.length + (uint32_t)(entBlob ? entBlob.length : 0);

	NSMutableData *sb = [NSMutableData dataWithCapacity:sbLen];
	uint32_t sbHead[3] = {
		OSSwapHostToBigInt32(0xfade0cc0u),
		OSSwapHostToBigInt32(sbLen),
		OSSwapHostToBigInt32(blobCount),
	};
	[sb appendBytes:sbHead length:12];
	uint32_t idx0[2] = { OSSwapHostToBigInt32(0), OSSwapHostToBigInt32(sbHdr) };
	[sb appendBytes:idx0 length:8];
	if (entBlob) {
		uint32_t idx1[2] = { OSSwapHostToBigInt32(5), OSSwapHostToBigInt32(sbHdr + (uint32_t)cd.length) };
		[sb appendBytes:idx1 length:8];
	}
	[sb appendData:cd];
	if (entBlob) [sb appendData:entBlob];
	return sb;
}

static NSData *default_app_entitlements(NSString *bundleId)
{
	NSString *bid = bundleId.length > 0 ? bundleId : @"org.iosonpixel.app";
	NSDictionary *dict = @{
		@"application-identifier": [NSString stringWithFormat:@"0000000000.%@", bid],
		@"com.apple.developer.team-identifier": @"0000000000",
		@"get-task-allow": @YES,
		@"keychain-access-groups": @[
			[NSString stringWithFormat:@"0000000000.%@", bid],
			bid,
			@"*"
		],
	};
	return [NSPropertyListSerialization dataWithPropertyList:dict
	                                                  format:NSPropertyListXMLFormat_v1_0
	                                                 options:0
	                                                   error:nil];
}

static BOOL prepare_macho_file(NSString *filePath, BOOL isMainExec, NSString *bundleId, NSString **errOut)
{
	const char *cpath = filePath.fileSystemRepresentation;
	struct stat st;
	if (lstat(cpath, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 64) {
		return YES;
	}

	int fd = open(cpath, O_RDONLY);
	if (fd < 0) return YES;
	uint32_t magic = 0;
	if (!pread_all(fd, &magic, 4, 0)) {
		close(fd);
		return YES;
	}

	/* Thin universal Mach-O (FAT_MAGIC / FAT_CIGAM) to plain arm64 slice */
	if (magic == FAT_MAGIC || magic == FAT_CIGAM) {
		struct fat_header fhdr;
		if (!pread_all(fd, &fhdr, sizeof(fhdr), 0)) { close(fd); return YES; }
		uint32_t narch = OSSwapBigToHostInt32(fhdr.nfat_arch);
		if (narch > 32) { close(fd); return YES; }
		uint64_t chosenOff = 0, chosenSize = 0;
		BOOL sawArm64e = NO;
		for (uint32_t i = 0; i < narch; i++) {
			struct fat_arch fa;
			if (!pread_all(fd, &fa, sizeof(fa), (off_t)(sizeof(fhdr) + i * sizeof(fa)))) break;
			cpu_type_t ct = (cpu_type_t)OSSwapBigToHostInt32((uint32_t)fa.cputype);
			cpu_subtype_t cs = (cpu_subtype_t)(OSSwapBigToHostInt32((uint32_t)fa.cpusubtype) & ~CPU_SUBTYPE_MASK);
			if (ct == CPU_TYPE_ARM64) {
				if (cs == CPU_SUBTYPE_ARM64_ALL) {
					chosenOff = OSSwapBigToHostInt32(fa.offset);
					chosenSize = OSSwapBigToHostInt32(fa.size);
					break;
				} else if (cs == 2 /* CPU_SUBTYPE_ARM64E */) {
					sawArm64e = YES;
				}
			}
		}
		if (chosenSize == 0) {
			close(fd);
			if (isMainExec && sawArm64e) {
				if (errOut) *errOut = @"App binary only contains an arm64e (PAC) slice and no arm64 slice.";
				return NO;
			}
			return YES;
		}
		NSString *tmpSlice = [filePath stringByAppendingString:@".arm64slice"];
		int outFd = open(tmpSlice.fileSystemRepresentation, O_CREAT | O_TRUNC | O_WRONLY, 0755);
		if (outFd < 0) { close(fd); return NO; }
		uint8_t buf[65536];
		uint64_t rem = chosenSize, cur = chosenOff;
		BOOL copyOk = YES;
		while (rem > 0) {
			size_t n = (size_t)MIN(rem, (uint64_t)sizeof(buf));
			if (!pread_all(fd, buf, n, (off_t)cur) || !write_all(outFd, buf, n)) { copyOk = NO; break; }
			cur += n; rem -= n;
		}
		close(outFd);
		close(fd);
		if (!copyOk || rename(tmpSlice.fileSystemRepresentation, cpath) != 0) {
			unlink(tmpSlice.fileSystemRepresentation);
			if (errOut) *errOut = @"Failed thinning universal binary to arm64";
			return NO;
		}
		chown(cpath, 0, 0);
		chmod(cpath, 0755);
		fd = open(cpath, O_RDONLY);
		if (fd < 0 || !pread_all(fd, &magic, 4, 0)) { if (fd >= 0) close(fd); return NO; }
		fstat(fd, &st);
	}

	if (magic != MH_MAGIC_64) {
		close(fd);
		return YES;
	}

	/* Ensure executable permission on all Mach-O binaries/dylibs */
	chown(cpath, 0, 0);
	chmod(cpath, 0755);

	struct mach_header_64 mhdr;
	if (!pread_all(fd, &mhdr, sizeof(mhdr), 0) || mhdr.sizeofcmds > (2u << 20)) {
		close(fd);
		return YES;
	}
	uint64_t fileSize = (uint64_t)st.st_size;
	uint8_t *cmds = (uint8_t *)malloc(mhdr.sizeofcmds);
	if (!cmds || !pread_all(fd, cmds, mhdr.sizeofcmds, sizeof(mhdr))) {
		free(cmds);
		close(fd);
		return YES;
	}

	uint64_t textFileOff = 0, textFileSize = 0;
	ssize_t linkeditCmdOff = -1, sigCmdOff = -1;
	uint32_t firstSectOff = 0xffffffffu;
	uint32_t cryptId = 0;

	size_t off = 0;
	for (uint32_t i = 0; i < mhdr.ncmds && off + sizeof(struct load_command) <= mhdr.sizeofcmds; i++) {
		struct load_command *lc = (struct load_command *)(cmds + off);
		if (lc->cmdsize < sizeof(struct load_command) || off + lc->cmdsize > mhdr.sizeofcmds) break;
		if (lc->cmd == LC_SEGMENT_64 && lc->cmdsize >= sizeof(struct segment_command_64)) {
			struct segment_command_64 *seg = (struct segment_command_64 *)lc;
			if (strncmp(seg->segname, "__TEXT", 16) == 0) {
				textFileOff = seg->fileoff;
				textFileSize = seg->filesize;
			} else if (strncmp(seg->segname, "__LINKEDIT", 16) == 0) {
				linkeditCmdOff = (ssize_t)off;
			}
			struct section_64 *sects = (struct section_64 *)(seg + 1);
			uint32_t maxSects = (lc->cmdsize - (uint32_t)sizeof(*seg)) / (uint32_t)sizeof(*sects);
			for (uint32_t s = 0; s < seg->nsects && s < maxSects; s++) {
				if (sects[s].size > 0 && sects[s].offset > 0 && sects[s].offset < firstSectOff) {
					firstSectOff = sects[s].offset;
				}
			}
		} else if (lc->cmd == LC_ENCRYPTION_INFO_64 && lc->cmdsize >= sizeof(struct encryption_info_command_64)) {
			struct encryption_info_command_64 *enc = (struct encryption_info_command_64 *)lc;
			if (enc->cryptid != 0) cryptId = enc->cryptid;
		} else if (lc->cmd == LC_ENCRYPTION_INFO && lc->cmdsize >= sizeof(struct encryption_info_command)) {
			struct encryption_info_command *enc = (struct encryption_info_command *)lc;
			if (enc->cryptid != 0) cryptId = enc->cryptid;
		} else if (lc->cmd == LC_CODE_SIGNATURE && lc->cmdsize >= sizeof(struct linkedit_data_command)) {
			sigCmdOff = (ssize_t)off;
		}
		off += lc->cmdsize;
	}

	if (cryptId != 0) {
		free(cmds);
		close(fd);
		if (errOut) {
			*errOut = @"Encrypted App Store IPA (FairPlay DRM cryptid=1). Please use a decrypted .ipa (from a repo source, Decrypt.day, or GitHub Releases).";
		}
		return NO;
	}

	BOOL hasValidCD = NO;
	NSData *existingXmlEnts = nil;
	uint32_t sigOff = 0, sigSize = 0;
	if (sigCmdOff >= 0) {
		struct linkedit_data_command *sc = (struct linkedit_data_command *)(cmds + sigCmdOff);
		sigOff = sc->dataoff;
		sigSize = sc->datasize;
		if (sigSize >= 20 && (uint64_t)sigOff + sigSize <= fileSize) {
			uint8_t *sb = (uint8_t *)malloc(sigSize);
			if (sb && pread_all(fd, sb, sigSize, (off_t)sigOff)) {
				uint32_t sbMagic = OSSwapBigToHostInt32(*(uint32_t *)sb);
				uint32_t sbCount = OSSwapBigToHostInt32(*(uint32_t *)(sb + 8));
				if (sbMagic == 0xfade0cc0u && sbCount <= 32 && 12 + sbCount * 8 <= sigSize) {
					for (uint32_t b = 0; b < sbCount; b++) {
						uint32_t btype = OSSwapBigToHostInt32(*(uint32_t *)(sb + 12 + b * 8));
						uint32_t boff  = OSSwapBigToHostInt32(*(uint32_t *)(sb + 16 + b * 8));
						if (boff + 8 > sigSize) continue;
						uint32_t bmagic = OSSwapBigToHostInt32(*(uint32_t *)(sb + boff));
						uint32_t blen   = OSSwapBigToHostInt32(*(uint32_t *)(sb + boff + 4));
						if (boff + blen > sigSize || blen < 8) continue;
						if ((btype == 0 || (btype >= 0x1000 && btype < 0x1010)) && bmagic == 0xfade0c02u) {
							hasValidCD = YES;
						} else if (btype == 5 && bmagic == 0xfade7171u && blen > 8) {
							existingXmlEnts = [NSData dataWithBytes:(sb + boff + 8) length:(blen - 8)];
						}
					}
				}
			}
			free(sb);
		}
	}
	close(fd);

	/* If already signed with a valid CodeDirectory and (for main binary) XML entitlements, keep as-is! */
	if (hasValidCD && (!isMainExec || existingXmlEnts.length > 0)) {
		free(cmds);
		return YES;
	}

	if (linkeditCmdOff < 0) {
		free(cmds);
		return YES;
	}

	/* Embed ad-hoc SHA-256 signature + XML entitlements */
	int rwFd = open(cpath, O_RDWR);
	if (rwFd < 0) {
		free(cmds);
		return YES;
	}

	uint64_t codeLimit = fileSize;
	BOOL addedSigCmd = NO;
	struct linkedit_data_command newSigCmd = { LC_CODE_SIGNATURE, sizeof(struct linkedit_data_command), 0, 0 };

	if (sigCmdOff >= 0) {
		struct linkedit_data_command *sc = (struct linkedit_data_command *)(cmds + sigCmdOff);
		if ((uint64_t)sc->dataoff + sc->datasize == fileSize || sc->datasize == 0 || !hasValidCD) {
			codeLimit = sc->dataoff;
		} else {
			/* Signature is not at EOF; leave existing valid CD untouched */
			close(rwFd);
			free(cmds);
			return YES;
		}
	} else {
		if (firstSectOff == 0xffffffffu) firstSectOff = 4096;
		if (sizeof(mhdr) + mhdr.sizeofcmds + sizeof(newSigCmd) > firstSectOff) {
			close(rwFd);
			free(cmds);
			return YES;
		}
		codeLimit = (fileSize + 15ULL) & ~15ULL;
		if (codeLimit > fileSize) {
			uint8_t pad[16] = { 0 };
			(void)pwrite(rwFd, pad, (size_t)(codeLimit - fileSize), (off_t)fileSize);
		}
		addedSigCmd = YES;
	}

	struct segment_command_64 *le = (struct segment_command_64 *)(cmds + linkeditCmdOff);
	if (codeLimit < le->fileoff) {
		close(rwFd);
		free(cmds);
		return YES;
	}

	/* Write updated header + preliminary load commands BEFORE hashing page 0!
	 * Because superblob length depends only on codeLimit,ident,xmlEnts (NOT page contents),
	 * we can compute exact datasize & __LINKEDIT sizes first, write header+cmds to page 0,
	 * then hash all pages [0..codeLimit) accurately! */
	NSData *xmlEnts = existingXmlEnts ?: (isMainExec ? default_app_entitlements(bundleId) : nil);
	NSString *ident = isMainExec ? bundleId : [filePath.lastPathComponent stringByDeletingPathExtension];

	/* Calculate exact superblob size deterministically */
	uint32_t nCodeSlots = (uint32_t)((codeLimit + 4095ULL) / 4096ULL);
	uint32_t nSpecial = xmlEnts.length > 0 ? 5 : 0;
	const char *identC = ident.length > 0 ? ident.UTF8String : "org.iosonpixel.app";
	uint32_t cdLen = 88 + (uint32_t)strlen(identC) + 1 + (nSpecial + nCodeSlots) * 32;
	uint32_t entBlobLen = xmlEnts.length > 0 ? (uint32_t)(8 + xmlEnts.length) : 0;
	uint32_t blobCount = entBlobLen > 0 ? 2 : 1;
	uint32_t exactSigLen = 12 + blobCount * 8 + cdLen + entBlobLen;

	le->filesize = (codeLimit - le->fileoff) + exactSigLen;
	uint64_t minVm = (le->filesize + 0x3fffULL) & ~0x3fffULL;
	if (le->vmsize < minVm) le->vmsize = minVm;

	if (addedSigCmd) {
		newSigCmd.dataoff = (uint32_t)codeLimit;
		newSigCmd.datasize = exactSigLen;
		uint32_t oldCmdsLen = mhdr.sizeofcmds;
		mhdr.ncmds += 1;
		mhdr.sizeofcmds += sizeof(newSigCmd);
		(void)pwrite(rwFd, &mhdr, sizeof(mhdr), 0);
		(void)pwrite(rwFd, cmds, oldCmdsLen, sizeof(mhdr));
		(void)pwrite(rwFd, &newSigCmd, sizeof(newSigCmd), (off_t)(sizeof(mhdr) + oldCmdsLen));
	} else {
		struct linkedit_data_command *sc = (struct linkedit_data_command *)(cmds + sigCmdOff);
		sc->dataoff = (uint32_t)codeLimit;
		sc->datasize = exactSigLen;
		(void)pwrite(rwFd, cmds, mhdr.sizeofcmds, sizeof(mhdr));
	}

	NSData *sb = build_adhoc_superblob(rwFd, codeLimit, textFileOff, textFileSize,
	                                   mhdr.filetype == MH_EXECUTE, ident, xmlEnts);
	if (sb.length == exactSigLen) {
		(void)ftruncate(rwFd, (off_t)codeLimit);
		(void)pwrite(rwFd, sb.bytes, sb.length, (off_t)codeLimit);
	}
	close(rwFd);
	free(cmds);
	return YES;
}

/* ---- Install / Uninstall jobs ---- */

static void install_ipa_worker(NSString *ipaPath, BOOL deleteAfter, NSString *hintName)
{
	@autoreleasepool {
		NSFileManager *fm = [NSFileManager defaultManager];
		NSString *stageDir = [NSString stringWithFormat:@"%s/job-%llu",
		    STAGE_ROOT, (unsigned long long)([[NSDate date] timeIntervalSince1970] * 1000.0)];
		[fm removeItemAtPath:stageDir error:nil];
		[fm createDirectoryAtPath:stageDir withIntermediateDirectories:YES attributes:nil error:nil];

		NSString *err = nil;
		set_job(@"extracting", 0.08, @"Extracting IPA archive...", hintName ?: @"", @"", @"");
		if (!extract_ipa_zip(ipaPath, stageDir, 0.08, 0.58, &err)) {
			[fm removeItemAtPath:stageDir error:nil];
			if (deleteAfter) [fm removeItemAtPath:ipaPath error:nil];
			set_job(@"error", 0.0, @"Extraction failed", hintName ?: @"", @"", err ?: @"Failed to unpack IPA");
			pthread_mutex_lock(&g_job_lock); g_busy = NO; pthread_mutex_unlock(&g_job_lock);
			return;
		}

		NSString *payloadDir = [stageDir stringByAppendingPathComponent:@"Payload"];
		NSArray<NSString *> *items = [fm contentsOfDirectoryAtPath:payloadDir error:nil];
		NSString *appFolder = nil;
		for (NSString *it in items) {
			if ([it.pathExtension.lowercaseString isEqualToString:@"app"]) {
				appFolder = it;
				break;
			}
		}
		if (!appFolder) {
			[fm removeItemAtPath:stageDir error:nil];
			if (deleteAfter) [fm removeItemAtPath:ipaPath error:nil];
			set_job(@"error", 0.0, @"Invalid IPA", hintName ?: @"", @"", @"No Payload/*.app bundle found inside archive");
			pthread_mutex_lock(&g_job_lock); g_busy = NO; pthread_mutex_unlock(&g_job_lock);
			return;
		}

		NSString *stagedApp = [payloadDir stringByAppendingPathComponent:appFolder];
		NSString *infoPath = [stagedApp stringByAppendingPathComponent:@"Info.plist"];
		NSDictionary *info = [NSDictionary dictionaryWithContentsOfFile:infoPath];
		NSString *bundleId = [info[@"CFBundleIdentifier"] isKindOfClass:[NSString class]] ? info[@"CFBundleIdentifier"] : @"";
		NSString *execName = [info[@"CFBundleExecutable"] isKindOfClass:[NSString class]] ? info[@"CFBundleExecutable"] : @"";
		NSString *dispName = [info[@"CFBundleDisplayName"] isKindOfClass:[NSString class]] ? info[@"CFBundleDisplayName"] :
		                     ([info[@"CFBundleName"] isKindOfClass:[NSString class]] ? info[@"CFBundleName"] :
		                     [appFolder stringByDeletingPathExtension]);
		NSString *version  = [info[@"CFBundleShortVersionString"] isKindOfClass:[NSString class]] ? info[@"CFBundleShortVersionString"] :
		                     ([info[@"CFBundleVersion"] isKindOfClass:[NSString class]] ? info[@"CFBundleVersion"] : @"1.0");

		if (bundleId.length == 0 || execName.length == 0) {
			[fm removeItemAtPath:stageDir error:nil];
			if (deleteAfter) [fm removeItemAtPath:ipaPath error:nil];
			set_job(@"error", 0.0, @"Invalid Info.plist", dispName, @"", @"Missing CFBundleIdentifier or CFBundleExecutable");
			pthread_mutex_lock(&g_job_lock); g_busy = NO; pthread_mutex_unlock(&g_job_lock);
			return;
		}

		set_job(@"signing", 0.62, [NSString stringWithFormat:@"Verifying & signing %@...", dispName], dispName, bundleId, @"");
		chown_chmod_recursive(stagedApp);

		NSString *mainExecPath = [stagedApp stringByAppendingPathComponent:execName];
		if (![fm fileExistsAtPath:mainExecPath]) {
			[fm removeItemAtPath:stageDir error:nil];
			if (deleteAfter) [fm removeItemAtPath:ipaPath error:nil];
			set_job(@"error", 0.0, @"Missing executable", dispName, bundleId,
			    [NSString stringWithFormat:@"Executable %@ missing from app bundle", execName]);
			pthread_mutex_lock(&g_job_lock); g_busy = NO; pthread_mutex_unlock(&g_job_lock);
			return;
		}

		if (!prepare_macho_file(mainExecPath, YES, bundleId, &err)) {
			[fm removeItemAtPath:stageDir error:nil];
			if (deleteAfter) [fm removeItemAtPath:ipaPath error:nil];
			set_job(@"error", 0.0, @"Cannot install binary", dispName, bundleId, err ?: @"Incompatible binary");
			pthread_mutex_lock(&g_job_lock); g_busy = NO; pthread_mutex_unlock(&g_job_lock);
			return;
		}

		NSDirectoryEnumerator *en = [fm enumeratorAtPath:stagedApp];
		NSString *rel;
		while ((rel = [en nextObject]) != nil) {
			NSString *full = [stagedApp stringByAppendingPathComponent:rel];
			if ([full isEqualToString:mainExecPath]) continue;
			if (!prepare_macho_file(full, NO, bundleId, &err)) {
				[fm removeItemAtPath:stageDir error:nil];
				if (deleteAfter) [fm removeItemAtPath:ipaPath error:nil];
				set_job(@"error", 0.0, @"Framework verification failed", dispName, bundleId, err ?: @"Encrypted framework");
				pthread_mutex_lock(&g_job_lock); g_busy = NO; pthread_mutex_unlock(&g_job_lock);
				return;
			}
		}

		/* Choose safe destination in /Applications without clobbering stock Apple apps */
		NSString *destDirName = appFolder;
		NSString *destApp = [@"/Applications" stringByAppendingPathComponent:destDirName];
		if ([fm fileExistsAtPath:destApp] &&
		    ![fm fileExistsAtPath:[destApp stringByAppendingPathComponent:@MARKER_FILE]] &&
		    ![bundleId isEqualToString:@"org.iosonpixel.sweetstore"]) {
			NSString *safeBid = [[bundleId componentsSeparatedByCharactersInSet:
			    [[NSCharacterSet alphanumericCharacterSet] invertedSet]] componentsJoinedByString:@"_"];
			destDirName = [NSString stringWithFormat:@"%@.app", safeBid];
			destApp = [@"/Applications" stringByAppendingPathComponent:destDirName];
		}

		NSDictionary *meta = @{
			@"name": dispName,
			@"bundleId": bundleId,
			@"version": version,
			@"dirName": destDirName,
			@"installedAt": @((long long)[[NSDate date] timeIntervalSince1970]),
		};
		NSString *markerPath = [stagedApp stringByAppendingPathComponent:@MARKER_FILE];
		[meta writeToFile:markerPath atomically:YES];
		chown(markerPath.fileSystemRepresentation, 0, 0);
		chmod(markerPath.fileSystemRepresentation, 0644);

		set_job(@"installing", 0.85, [NSString stringWithFormat:@"Installing %@ to /Applications...", dispName], dispName, bundleId, @"");
		NSString *oldBackup = [destApp stringByAppendingString:@".old"];
		[fm removeItemAtPath:oldBackup error:nil];
		if ([fm fileExistsAtPath:destApp]) {
			[fm moveItemAtPath:destApp toPath:oldBackup error:nil];
		}
		NSError *mvErr = nil;
		if (![fm moveItemAtPath:stagedApp toPath:destApp error:&mvErr]) {
			if ([fm fileExistsAtPath:oldBackup]) [fm moveItemAtPath:oldBackup toPath:destApp error:nil];
			[fm removeItemAtPath:stageDir error:nil];
			if (deleteAfter) [fm removeItemAtPath:ipaPath error:nil];
			set_job(@"error", 0.0, @"Move to /Applications failed", dispName, bundleId, mvErr.localizedDescription ?: @"Move failed");
			pthread_mutex_lock(&g_job_lock); g_busy = NO; pthread_mutex_unlock(&g_job_lock);
			return;
		}
		[fm removeItemAtPath:oldBackup error:nil];
		[fm removeItemAtPath:stageDir error:nil];
		if (deleteAfter) [fm removeItemAtPath:ipaPath error:nil];

		set_job(@"installing", 0.92, @"Registering icon on Home Screen (lsreg)...", dispName, bundleId, @"");
		(void)run_lsreg_rebuild();

		set_job(@"done", 1.0, [NSString stringWithFormat:@"Installed %@ (%@)", dispName, version], dispName, bundleId, @"");
		fprintf(stderr, "sweetstored: installed %s (%s) -> %s\n",
		    dispName.UTF8String, bundleId.UTF8String, destApp.UTF8String);
		pthread_mutex_lock(&g_job_lock); g_busy = NO; pthread_mutex_unlock(&g_job_lock);
	}
}

/* ---- Scans & HTTP API ---- */

static NSArray *scan_installed_apps(void)
{
	NSFileManager *fm = [NSFileManager defaultManager];
	NSArray<NSString *> *apps = [fm contentsOfDirectoryAtPath:@"/Applications" error:nil];
	NSMutableArray *list = [NSMutableArray array];
	for (NSString *dir in apps) {
		if (![dir.pathExtension.lowercaseString isEqualToString:@"app"]) continue;
		NSString *full = [@"/Applications" stringByAppendingPathComponent:dir];
		NSString *marker = [full stringByAppendingPathComponent:@MARKER_FILE];
		if (![fm fileExistsAtPath:marker]) continue;
		NSDictionary *meta = [NSDictionary dictionaryWithContentsOfFile:marker];
		if (!meta) continue;
		[list addObject:@{
			@"name": meta[@"name"] ?: [dir stringByDeletingPathExtension],
			@"bundleId": meta[@"bundleId"] ?: @"",
			@"version": meta[@"version"] ?: @"1.0",
			@"dirName": dir,
			@"path": full,
		}];
	}
	[list sortUsingComparator:^NSComparisonResult(NSDictionary *a, NSDictionary *b) {
		return [a[@"name"] localizedCaseInsensitiveCompare:b[@"name"]];
	}];
	return list;
}

static NSArray *scan_local_ipas(void)
{
	NSFileManager *fm = [NSFileManager defaultManager];
	NSArray<NSString *> *dirs = @[
		@"/private/var/mobile/Downloads",
		@"/private/var/mobile/Media/Downloads",
		@"/private/var/mobile/Documents",
		@LOCAL_IPA_DIR,
	];
	NSMutableArray *found = [NSMutableArray array];
	NSMutableSet *seen = [NSMutableSet set];
	for (NSString *d in dirs) {
		NSArray<NSString *> *files = [fm contentsOfDirectoryAtPath:d error:nil];
		for (NSString *f in files) {
			if (![f.pathExtension.lowercaseString isEqualToString:@"ipa"]) continue;
			if ([f hasPrefix:@".sweetdl-"]) continue;
			NSString *full = [d stringByAppendingPathComponent:f];
			if ([seen containsObject:full]) continue;
			[seen addObject:full];
			NSDictionary *attr = [fm attributesOfItemAtPath:full error:nil];
			unsigned long long sz = [attr fileSize];
			if (sz < 64) continue;
			[found addObject:@{
				@"name": f,
				@"path": full,
				@"size": @(sz),
			}];
		}
	}
	return found;
}

static NSDictionary *build_status_dict(void)
{
	pthread_mutex_lock(&g_job_lock);
	BOOL busy = g_busy;
	NSDictionary *job = @{
		@"state": g_state ?: @"idle",
		@"progress": @(g_progress),
		@"step": g_step ?: @"",
		@"appName": g_app_name ?: @"",
		@"bundleId": g_bundle_id ?: @"",
		@"error": g_error ?: @"",
	};
	pthread_mutex_unlock(&g_job_lock);
	return @{
		@"ok": @YES,
		@"busy": @(busy),
		@"job": job,
		@"installed": scan_installed_apps(),
		@"localIpas": scan_local_ipas(),
	};
}

static void send_json(int cfd, int status, NSDictionary *obj)
{
	NSData *body = [NSJSONSerialization dataWithJSONObject:obj options:0 error:nil] ?: [NSData data];
	const char *stText = (status == 200) ? "OK" : (status == 409 ? "Conflict" : "Bad Request");
	char hdr[256];
	int hlen = snprintf(hdr, sizeof(hdr),
	    "HTTP/1.1 %d %s\r\n"
	    "Content-Type: application/json; charset=utf-8\r\n"
	    "Content-Length: %lu\r\n"
	    "Connection: close\r\n\r\n",
	    status, stText, (unsigned long)body.length);
	(void)write_all(cfd, hdr, (size_t)hlen);
	if (body.length > 0) (void)write_all(cfd, body.bytes, body.length);
}

static void handle_client(int cfd)
{
	@autoreleasepool {
		int one = 1;
		setsockopt(cfd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
		struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
		setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

		NSMutableData *req = [NSMutableData dataWithCapacity:4096];
		uint8_t buf[2048];
		ssize_t hdrEnd = -1;
		size_t contentLen = 0;

		while (req.length < 65536) {
			ssize_t n = recv(cfd, buf, sizeof(buf), 0);
			if (n <= 0) break;
			[req appendBytes:buf length:(NSUInteger)n];
			if (hdrEnd < 0) {
				const char *b = (const char *)req.bytes;
				for (size_t i = 0; i + 3 < req.length; i++) {
					if (b[i] == '\r' && b[i+1] == '\n' && b[i+2] == '\r' && b[i+3] == '\n') {
						hdrEnd = (ssize_t)(i + 4);
						NSString *hs = [[NSString alloc] initWithBytes:b length:i encoding:NSISOLatin1StringEncoding];
						for (NSString *line in [hs componentsSeparatedByString:@"\r\n"]) {
							if ([line.lowercaseString hasPrefix:@"content-length:"]) {
								contentLen = (size_t)[[line substringFromIndex:15] integerValue];
							}
						}
						break;
					}
				}
			}
			if (hdrEnd >= 0 && req.length >= (NSUInteger)hdrEnd + contentLen) break;
		}
		if (hdrEnd < 0) { close(cfd); return; }

		NSString *firstLine = [[[NSString alloc] initWithBytes:req.bytes length:(NSUInteger)(hdrEnd - 4)
		                                              encoding:NSISOLatin1StringEncoding]
		                       componentsSeparatedByString:@"\r\n"].firstObject ?: @"";
		NSArray<NSString *> *parts = [firstLine componentsSeparatedByString:@" "];
		NSString *method = parts.count > 0 ? parts[0] : @"";
		NSString *path   = parts.count > 1 ? parts[1] : @"";

		NSDictionary *json = nil;
		if (contentLen > 0 && req.length >= (NSUInteger)hdrEnd + contentLen) {
			NSData *body = [req subdataWithRange:NSMakeRange((NSUInteger)hdrEnd, contentLen)];
			id parsed = [NSJSONSerialization JSONObjectWithData:body options:0 error:nil];
			if ([parsed isKindOfClass:[NSDictionary class]]) json = parsed;
		}

		if ([method isEqualToString:@"GET"] && [path hasPrefix:@"/status"]) {
			send_json(cfd, 200, build_status_dict());
			close(cfd);
			return;
		}

		if ([method isEqualToString:@"POST"] && [path isEqualToString:@"/install-file"]) {
			NSString *ipaPath = [json[@"path"] isKindOfClass:[NSString class]] ? json[@"path"] : @"";
			BOOL delAfter = [json[@"deleteAfter"] respondsToSelector:@selector(boolValue)] ? [json[@"deleteAfter"] boolValue] : NO;
			NSString *disp = [json[@"displayName"] isKindOfClass:[NSString class]] ? json[@"displayName"] : @"";
			if (ipaPath.length == 0 || ![[NSFileManager defaultManager] fileExistsAtPath:ipaPath]) {
				send_json(cfd, 400, @{ @"ok": @NO, @"error": @"IPA file not found on disk" });
				close(cfd);
				return;
			}
			pthread_mutex_lock(&g_job_lock);
			if (g_busy) {
				pthread_mutex_unlock(&g_job_lock);
				send_json(cfd, 409, @{ @"ok": @NO, @"error": @"Another installation is already in progress" });
				close(cfd);
				return;
			}
			g_busy = YES;
			pthread_mutex_unlock(&g_job_lock);
			set_job(@"extracting", 0.05, @"Starting installation...", disp, @"", @"");
			dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
				install_ipa_worker(ipaPath, delAfter, disp);
			});
			send_json(cfd, 200, @{ @"ok": @YES });
			close(cfd);
			return;
		}

		if ([method isEqualToString:@"POST"] && [path isEqualToString:@"/uninstall"]) {
			NSString *dirName = [json[@"dirName"] isKindOfClass:[NSString class]] ? json[@"dirName"] : @"";
			if (dirName.length == 0 || [dirName containsString:@"/"] || [dirName containsString:@".."]) {
				send_json(cfd, 400, @{ @"ok": @NO, @"error": @"Invalid bundle directory" });
				close(cfd);
				return;
			}
			NSString *appPath = [@"/Applications" stringByAppendingPathComponent:dirName];
			NSString *marker = [appPath stringByAppendingPathComponent:@MARKER_FILE];
			NSFileManager *fm = [NSFileManager defaultManager];
			if (![fm fileExistsAtPath:marker]) {
				send_json(cfd, 400, @{ @"ok": @NO, @"error": @"Refusing to uninstall system app" });
				close(cfd);
				return;
			}
			[fm removeItemAtPath:appPath error:nil];
			(void)run_lsreg_rebuild();
			send_json(cfd, 200, @{ @"ok": @YES, @"installed": scan_installed_apps() });
			close(cfd);
			return;
		}

		if ([method isEqualToString:@"POST"] && [path isEqualToString:@"/rebuild-icons"]) {
			int rc = run_lsreg_rebuild();
			send_json(cfd, 200, @{ @"ok": @(rc == 0), @"rc": @(rc) });
			close(cfd);
			return;
		}

		if ([method isEqualToString:@"POST"] && [path isEqualToString:@"/open"]) {
			NSString *bid = [json[@"bundleId"] isKindOfClass:[NSString class]] ? json[@"bundleId"] : @"";
			if (bid.length == 0) {
				send_json(cfd, 400, @{ @"ok": @NO, @"error": @"Missing bundleId" });
				close(cfd);
				return;
			}
			dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^{
				char *const argv[] = { (char *)"/var/local/bin/openapp", (char *)bid.UTF8String, NULL };
				(void)run_tool("/var/local/bin/openapp", argv);
			});
			send_json(cfd, 200, @{ @"ok": @YES });
			close(cfd);
			return;
		}

		send_json(cfd, 400, @{ @"ok": @NO, @"error": @"Unknown endpoint" });
		close(cfd);
	}
}

static void ensure_first_boot_registration(void)
{
	@autoreleasepool {
		struct stat st;
		if (stat("/Applications/SweetStore.app/SweetStore", &st) != 0) return;
		NSString *want = [NSString stringWithFormat:@"%lld:%lld\n",
		    (long long)st.st_size, (long long)st.st_mtimespec.tv_sec];
		NSString *have = [NSString stringWithContentsOfFile:@REG_STAMP_PATH encoding:NSUTF8StringEncoding error:nil];
		if ([have isEqualToString:want]) return;

		/* Wait for lsd & SpringBoard to come up on boot, then rebuild LaunchServices once */
		for (int attempt = 0; attempt < 4; attempt++) {
			sleep(attempt == 0 ? 8 : 6);
			int rc = run_lsreg_rebuild();
			fprintf(stderr, "sweetstored: auto-register SweetStore.app attempt %d -> rc=%d\n", attempt + 1, rc);
			if (rc == 0) {
				[want writeToFile:@REG_STAMP_PATH atomically:YES encoding:NSUTF8StringEncoding error:nil];
				break;
			}
		}
	}
}

int main(int argc, char **argv)
{
	(void)argc; (void)argv;
	signal(SIGPIPE, SIG_IGN);
	@autoreleasepool {
		mkdir("/private/var/local", 0777);
		chmod("/private/var/local", 0777);
		mkdir(LOCAL_IPA_DIR, 0777);
		chmod(LOCAL_IPA_DIR, 0777);
		mkdir(STAGE_ROOT, 0755);
		mkdir("/private/var/mobile/Downloads", 0777);
		chown("/private/var/mobile/Downloads", 501, 501);
		chmod("/private/var/mobile/Downloads", 0777);

		dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
			ensure_first_boot_registration();
		});

		int srv = socket(AF_INET, SOCK_STREAM, 0);
		if (srv < 0) {
			perror("sweetstored: socket");
			return 1;
		}
		int one = 1;
		setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

		struct sockaddr_in addr;
		memset(&addr, 0, sizeof(addr));
		addr.sin_len = sizeof(addr);
		addr.sin_family = AF_INET;
		addr.sin_port = htons(SWEETSTORED_PORT);
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

		if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(srv, 16) != 0) {
			perror("sweetstored: bind/listen");
			close(srv);
			return 1;
		}
		fprintf(stderr, "sweetstored: listening on 127.0.0.1:%d\n", SWEETSTORED_PORT);

		for (;;) {
			int cfd = accept(srv, NULL, NULL);
			if (cfd < 0) {
				if (errno == EINTR) continue;
				usleep(100000);
				continue;
			}
			dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
				handle_client(cfd);
			});
		}
	}
}
