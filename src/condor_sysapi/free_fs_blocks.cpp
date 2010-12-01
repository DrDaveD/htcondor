/***************************************************************
 *
 * Copyright (C) 1990-2007, Condor Team, Computer Sciences Department,
 * University of Wisconsin-Madison, WI.
 * 
 * Licensed under the Apache License, Version 2.0 (the "License"); you
 * may not use this file except in compliance with the License.  You may
 * obtain a copy of the License at
 * 
 *    http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 ***************************************************************/


#include "condor_common.h"


/*
** free_fs_blocks: reports the number of kbytes free for the filesystem where
** given filename resides.
*/

#include "sysapi.h"
#include "sysapi_externs.h"
#include "my_popen.h"

/* static function declarations */
static int reserve_for_afs_cache(void);
static int reserve_for_fs(void);

/* the code starts here */

#if defined(WIN32)

#include <limits.h>

int
sysapi_disk_space_raw(const char *filename)
{
	ULARGE_INTEGER FreeBytesAvailableToCaller, TotalNumberOfBytes, TotalNumberOfFreeBytes;
	unsigned int t_hi, t_lo, temp;
	const unsigned int lowest_ten_mask = 0x00000cff;
	
	t_hi = t_lo = temp = 0;
	sysapi_internal_reconfig();

	
	if (GetDiskFreeSpaceEx(filename, &FreeBytesAvailableToCaller, &TotalNumberOfBytes, 
		&TotalNumberOfFreeBytes) == 0) {
		return -1;
	} else {
		
		// we have to shift everything down 10 bits since we report the number
		// of kbytes free. There's a HighPart and a LowPart, so it gets a little ugly.

		// first shift the low part (divide by 1024 to get it into kbytes)
		t_lo = FreeBytesAvailableToCaller.LowPart;
		t_lo = t_lo >> 10;
		// now grab the lowest 10 bits in the high part
		t_hi = FreeBytesAvailableToCaller.HighPart;
		temp = lowest_ten_mask & t_hi;
		// shift the high part 10 bits to get it into kbytes
		t_hi = t_hi >> 10;
		
		// shift the lowest 10 bits from the high part up to the 
		// highest 10 bits, and stick them into the low part
		temp = temp << 22;
		t_lo |= temp;

		if (t_hi > 0) {
			return INT_MAX;
		} else {
			return (int)t_lo;
		}
	}
}

#else

#include "condor_debug.h"

/* Can't include condor_config.h since it depends on classads which is
   only C++.  -Derek 6/25/98 */
extern char* param();


#define FS_PROGRAM "/usr/afsws/bin/fs"
#define FS_COMMAND "getcacheparms"
#define FS_OUTPUT_FORMAT "\nAFS using %d of the cache's available %d"

#endif

/*
  How much disk space we need to reserve for the AFS cache.  Answer is
  in kilobytes.
*/
static int
reserve_for_afs_cache()
{
#ifdef WIN32
	return 0;
#else
	int		answer;
	FILE	*fp;
	char	*args[] = {FS_PROGRAM, FS_COMMAND, NULL};
	int		cache_size, cache_in_use;
	int		do_it;

	/* See if we're configured to deal with an AFS cache */
	do_it = _sysapi_reserve_afs_cache;

		/* If we're not configured to deal with AFS cache, just return 0 */
	if( !do_it ) {
		return 0;
	}

		/* Run an AFS utility program to learn how big the cache is and
		   how much is in use. */
	dprintf( D_FULLDEBUG, "Checking AFS cache parameters\n" );
	fp = my_popenv( args, "r", FALSE );
	fscanf( fp, FS_OUTPUT_FORMAT, &cache_in_use, &cache_size );
	my_pclose( fp );
	dprintf( D_FULLDEBUG, "cache_in_use = %d, cache_size = %d\n",
		cache_in_use,
		cache_size
	);
	answer = cache_size - cache_in_use;

		/* The cache may be temporarily over size, in this case AFS will
		   clean up itself, so we don't have to allow for that. */
	if( answer < 0 ) {
		answer = 0;
	}

	dprintf( D_FULLDEBUG, "Reserving %d kbytes for AFS cache\n", answer );
	return answer;
#endif
}

/*
  How much disk space we need to reserve for the regular file system.
  Answer is in kilobytes.
*/
static int
reserve_for_fs()
{
	/* XXX make cleaner */
	int	answer = -1;

	if( answer < 0 ) {
		answer = _sysapi_reserve_disk;
		if( answer < 0 ) {
			answer = 0;
		}
	}

	answer = _sysapi_reserve_disk;
	return answer;
}


#if defined(__STDC__)
int sysapi_disk_space_raw( const char *filename);
#else
int sysapi_disk_space_raw();
#endif

#if defined(LINUX) || defined(AIX) || defined(HPUX) || defined(Solaris) || defined(Darwin) || defined(CONDOR_FREEBSD)

#include <limits.h>

#if defined(AIX)
#include <sys/statfs.h>
#elif defined(Solaris)
#include <sys/statvfs.h>
#endif

int sysapi_disk_space_raw(const char * filename)
{
#if defined(Solaris)
	struct statvfs statfsbuf;
#else
	struct statfs statfsbuf;
#endif
	double free_kbytes;
	float kbytes_per_block;

	sysapi_internal_reconfig();

#if defined(Solaris)
	if(statvfs(filename, &statfsbuf) < 0) {
#elif defined(AIX)
	if(statfs((char *)filename, &statfsbuf) < 0) {
#else
	if(statfs(filename, &statfsbuf) < 0) {
#endif
		if (errno != EOVERFLOW) {
			dprintf(D_ALWAYS, "sysapi_disk_space_raw: statfs(%s,%p) failed\n",
													filename, &statfsbuf );
			dprintf(D_ALWAYS, "errno = %d\n", errno );

			return(0);
		}

		// if we get here, it means that statfs failed because
		// there are more free blocks than can be represented
		// in a long.  Let's lie and make it INT_MAX - 1.

		dprintf(D_FULLDEBUG, "sysapi_disk_space_raw: statfs overflowed, setting to %d\n", (INT_MAX - 1));
		statfsbuf.f_bavail = (INT_MAX - 1);	
		statfsbuf.f_bsize = 1024; // this isn't set if result < 0, guess
	}

	/* Convert to kbyte blocks: available blks * blksize / 1k bytes. */
	/* overflow problem fixed by using doubles to hold result - 
		Keller 05/17/99 */

#if defined(Solaris)
		/* On Solaris, we need to use f_frsize, the "fundamental
		   filesystem block size", not f_bsize, the "preferred file
		   system block size".  3/25/98  Derek Wright */
	kbytes_per_block = ( (unsigned long)statfsbuf.f_frsize / 1024.0 );
#else
	kbytes_per_block = ( (unsigned long)statfsbuf.f_bsize / 1024.0 );
#endif

	free_kbytes = (double)statfsbuf.f_bavail * (double)kbytes_per_block; 
	if(free_kbytes > INT_MAX) {
		dprintf( D_ALWAYS, "sysapi_disk_space_raw: Free disk space kbytes overflow, capping to INT_MAX\n");
		return(INT_MAX);
	}

	dprintf( D_FULLDEBUG, "%.0f kbytes available for \"%s\"\n", 
			 free_kbytes, filename );

	return (int)free_kbytes;
}
#endif /* I386 && DYNIX */

/*
  Return number of kbytes condor may play with in the named file
  system.  System administrators may reserve space which condor should
  leave alone.  This could be either a static amount of space, or an
  amount of space based on the current size of the AFS cache (or
  both).

  Note: This code assumes that if we have an AFS cache, that cache is
  on the filesystem we are asking about.  This is not true in general,
  but it is the case at the developer's home site.  A more correct
  implementation would check whether the filesystem being asked about
  is the one containing the cache.  This is messy to do if the
  filesystem name we are given isn't a full pathname, e.g.
  sysapi_disk_space("."), which is why it isn't implemented here. -- mike

*/
int
sysapi_disk_space(const char *filename)
{
	int answer;

	sysapi_internal_reconfig();

	answer =  sysapi_disk_space_raw(filename)
			- reserve_for_afs_cache()
			- reserve_for_fs();
	return answer < 0 ? 0 : answer;

}


