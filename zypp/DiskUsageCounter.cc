/*---------------------------------------------------------------------\
|                          ____ _   __ __ ___                          |
|                         |__  / \ / / . \ . \                         |
|                           / / \ V /|  _/  _/                         |
|                          / /__ | | | | | |                           |
|                         /_____||_| |_| |_|                           |
|                                                                      |
\---------------------------------------------------------------------*/
/** \file	zypp/DiskUsageCounter.cc
 *
 */
extern "C"
{
#include <sys/statvfs.h>
}

#include <iostream>
#include <fstream>

#include "zypp/base/Easy.h"
#include "zypp/base/Logger.h"
#include "zypp/base/String.h"

#include "zypp/DiskUsageCounter.h"
#include "zypp/Package.h"

using std::endl;

///////////////////////////////////////////////////////////////////
namespace zypp
{ /////////////////////////////////////////////////////////////////

  ///////////////////////////////////////////////////////////////////
  namespace
  { /////////////////////////////////////////////////////////////////

    inline void addDu( DiskUsageCounter::MountPointSet & result_r, DiskUsage & du_r )
    {
      // traverse mountpoints in reverse order. This is done beacuse
      // DiskUsage::extract computes the mountpoint size, and then
      // removes the entry. So we must process leaves first.
      for_( mpit, result_r.rbegin(), result_r.rend() )
      {
        // Extract usage for the mount point
        DiskUsage::Entry entry = du_r.extract( mpit->dir );
        // Adjust the data.
        mpit->pkg_size += entry._size;
      }
    }

    inline void delDu( DiskUsageCounter::MountPointSet & result_r, DiskUsage & du_r )
    {
      // traverse mountpoints in reverse order. This is done beacuse
      // DiskUsage::extract computes the mountpoint size, and then
      // removes the entry. So we must process leaves first.
      for_( mpit, result_r.rbegin(), result_r.rend() )
      {
        // Extract usage for the mount point
        DiskUsage::Entry entry = du_r.extract( mpit->dir );
        // Adjust the data.
        mpit->pkg_size -= entry._size;
      }
    }

    /////////////////////////////////////////////////////////////////
  } // namespace
  ///////////////////////////////////////////////////////////////////

 DiskUsageCounter::MountPointSet DiskUsageCounter::disk_usage( const ResPool & pool_r )
  {
    DiskUsageCounter::MountPointSet result = mps;

    if ( result.empty() )
    {
      // partitioning is not set
      return result;
    }

    // set used size after commit to the current used size
    for_( it, result.begin(), result.end() )
    {
      it->pkg_size = it->used_size;
    }

    // iterate through all items
    for_( it, pool_r.begin(), pool_r.end() )
    {
      // skip items that do not transact
      if ( ! it->status().transacts() )
        continue;

      DiskUsage du( (*it)->diskusage() );

      // skip items without du info
      if ( du.empty() )
        continue; // or find some substitute info

      // Adjust the data.
      if ( it->status().isUninstalled() )
      {
        // an uninstalled item gets installed:
        addDu( result, du );

        // While there is no valid solver result, items to update
        // are selected, but installed old versions are not yet
        // deselected. We try to compensate this:
        if ( ! (*it)->installOnly() )
        {
          // Item to update -> check the installed ones.
          for_( nit, pool_r.byIdentBegin( *it ), pool_r.byIdentEnd( *it ) )
          {                                       // same kind and name
            if ( nit->status().staysInstalled() ) // and unselected installed
            {
              DiskUsage ndu( (*nit)->diskusage() );
              if ( ! ndu.empty() )
              {
                delDu( result, ndu );
              }
            }
          }
        }
      }
      else
      {
        // an installed item gets deleted:
        delDu( result, du );
      }
    }
    return result;
  }

  DiskUsageCounter::MountPointSet DiskUsageCounter::detectMountPoints(const std::string &rootdir)
  {
    DiskUsageCounter::MountPointSet ret;

      std::ifstream procmounts( "/proc/mounts" );

      if ( !procmounts ) {
	WAR << "Unable to read /proc/mounts" << std::endl;
      } else {

	std::string prfx;
	if ( rootdir != "/" )
	  prfx = rootdir; // rootdir not /

	while ( procmounts ) {
	  std::string l = str::getline( procmounts );
	  if ( !(procmounts.fail() || procmounts.bad()) ) {
	    // data to consume

	    // rootfs 	/ 		rootfs 		rw 0 0
	    // /dev/root 	/ 		reiserfs	rw 0 0
	    // proc 	/proc 		proc		rw 0 0
	    // devpts 	/dev/pts 	devpts		rw 0 0
	    // /dev/hda5 	/boot 		ext2		rw 0 0
	    // shmfs 	/dev/shm 	shm		rw 0 0
	    // usbdevfs 	/proc/bus/usb 	usbdevfs	rw 0 0

	    std::vector<std::string> words;
	    str::split( l, std::back_inserter(words) );

	    if ( words.size() < 3 ) {
	      WAR << "Suspicious entry in /proc/mounts: " << l << std::endl;
	      continue;
	    }

	    //
	    // Filter devices without '/' (proc,shmfs,..)
	    //
	    if ( words[0].find( '/' ) == std::string::npos ) {
	      DBG << "Discard mount point : " << l << std::endl;
	      continue;
	    }

	    //
	    // Filter mountpoints not at or below _rootdir
	    //
	    std::string mp = words[1];
	    if ( prfx.size() ) {
	      if ( mp.compare( 0, prfx.size(), prfx ) != 0 ) {
		// mountpoint not below rootdir
		DBG << "Unwanted mount point : " << l << std::endl;
		continue;
	      }
	      // strip prfx
	      mp.erase( 0, prfx.size() );
	      if ( mp.empty() ) {
		mp = "/";
	      } else if ( mp[0] != '/' ) {
		// mountpoint not below rootdir
		DBG << "Unwanted mount point : " << l << std::endl;
		continue;
	      }
	    }

	    //
	    // Filter cdrom
	    //
	    if ( words[2] == "iso9660" ) {
	      DBG << "Discard cdrom : " << l << std::endl;
	      continue;
	    }

	    if ( words[2] == "vfat" || words[2] == "fat" || words[2] == "ntfs" || words[2] == "ntfs-3g")
	    {
	      MIL << words[1] << " contains ignored fs (" << words[2] << ')' << std::endl;
	      continue;
	    }

	    //
	    // Filter some common unwanted mountpoints
	    //
	    const char * mpunwanted[] = {
	      "/mnt", "/media", "/mounts", "/floppy", "/cdrom",
	      "/suse", "/var/tmp", "/var/adm/mount", "/var/adm/YaST",
	      /*last*/0/*entry*/
	    };

	    const char ** nomp = mpunwanted;
	    for ( ; *nomp; ++nomp ) {
	      std::string pre( *nomp );
	      if ( mp.compare( 0, pre.size(), pre ) == 0 // mp has prefix pre
		   && ( mp.size() == pre.size() || mp[pre.size()] == '/' ) ) {
		break;
	      }
	    }
	    if ( *nomp ) {
	      DBG << "Filter mount point : " << l << std::endl;
	      continue;
	    }

	    //
	    // Check whether mounted readonly
	    //
	    bool ro = false;
	    std::vector<std::string> flags;
	    str::split( words[3], std::back_inserter(flags), "," );

	    for ( unsigned i = 0; i < flags.size(); ++i ) {
	      if ( flags[i] == "ro" ) {
		ro = true;
		break;
	      }
	    }
            if ( ro ) {
	      DBG << "Filter ro mount point : " << l << std::endl;
	      continue;
	    }

	    //
	    // statvfs (full path!) and get the data
	    //
	    struct statvfs sb;
	    if ( statvfs( words[1].c_str(), &sb ) != 0 ) {
	      WAR << "Unable to statvfs(" << words[1] << "); errno " << errno << std::endl;
	      ret.insert( DiskUsageCounter::MountPoint( mp ) );
	    }
	    else
	    {
	      ret.insert( DiskUsageCounter::MountPoint( mp, sb.f_bsize,
		((long long)sb.f_blocks)*sb.f_bsize/1024,
		((long long)(sb.f_blocks - sb.f_bfree))*sb.f_bsize/1024, 0LL, ro ) );
	    }
	  }
	}
    }

    return ret;
  }

  std::ostream & operator<<( std::ostream & str, const DiskUsageCounter::MountPoint & obj )
  {
    str << "dir:[" << obj.dir << "] [ bs: " << obj.block_size
        << " ts: " << obj.total_size
        << " us: " << obj.used_size
        << " (+-: " << (obj.pkg_size-obj.used_size)
        << ")]" << std::endl;
    return str;
  }

} // namespace zypp
///////////////////////////////////////////////////////////////////
