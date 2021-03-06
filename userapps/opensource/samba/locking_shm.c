/* 
   Unix SMB/Netbios implementation.
   Version 1.9.
   shared memory locking implementation
   Copyright (C) Andrew Tridgell 1992-1998
   
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
   
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
   
   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

   Revision History:

   12 aug 96: Erik.Devriendt@te6.siemens.be
   added support for shared memory implementation of share mode locking

   May 1997. Jeremy Allison (jallison@whistle.com). Modified share mode
   locking to deal with multiple share modes per open file.

   September 1997. Jeremy Allison (jallison@whistle.com). Added oplock
   support.

   October 1997 - split into separate file (tridge)
*/

#ifdef FAST_SHARE_MODES

#include "includes.h"
extern int DEBUGLEVEL;
extern connection_struct Connections[];
extern files_struct Files[];

static struct shmem_ops *shmops;

/* share mode record pointed to in shared memory hash bucket */
typedef struct
{
  int next_offset; /* offset of next record in chain from hash bucket */
  int locking_version;
  int32 st_dev;
  int32 st_ino;
  int num_share_mode_entries;
  int share_mode_entries; /* Chain of share mode entries for this file */
  char file_name[1];
} share_mode_record;

/* share mode entry pointed to by share_mode_record struct */
typedef struct
{
	int next_share_mode_entry;
	share_mode_entry e;
} shm_share_mode_entry;

static int read_only;


/* Conversion to hash entry index from device and inode numbers. */
#define HASH_ENTRY(dev,ino) ((((uint32)(dev)) ^ ((uint32)(ino))) % shmops->hash_size())


/*******************************************************************
  deinitialize the shared memory for share_mode management 
  ******************************************************************/
static BOOL shm_stop_share_mode_mgmt(void)
{
   return shmops->shm_close();
}

/*******************************************************************
  lock a hash bucket entry in shared memory for share_mode management 
  ******************************************************************/
static BOOL shm_lock_share_entry(int cnum, uint32 dev, uint32 inode, int *ptok)
{
  return shmops->lock_hash_entry(HASH_ENTRY(dev, inode));
}

/*******************************************************************
  unlock a hash bucket entry in shared memory for share_mode management 
  ******************************************************************/
static BOOL shm_unlock_share_entry(int cnum, uint32 dev, uint32 inode, int token)
{
  return shmops->unlock_hash_entry(HASH_ENTRY(dev, inode));
}

/*******************************************************************
get all share mode entries in shared memory for a dev/inode pair.
********************************************************************/
static int shm_get_share_modes(int cnum, int token, uint32 dev, uint32 inode, 
			       share_mode_entry **old_shares)
{
  int *mode_array;
  unsigned int hash_entry = HASH_ENTRY(dev, inode); 
  share_mode_record *file_scanner_p;
  share_mode_record *file_prev_p;
  shm_share_mode_entry *entry_scanner_p;
  shm_share_mode_entry *entry_prev_p;
  int num_entries;
  int num_entries_copied;
  BOOL found = False;
  share_mode_entry *share_array = (share_mode_entry *)0;

  *old_shares = 0;

  mode_array = (int *)shmops->offset2addr(shmops->get_userdef_off());
  
  if(mode_array[hash_entry] == 0)
  {
    DEBUG(5,("get_share_modes hash bucket %d empty\n", hash_entry));
    return 0;
  }

  file_scanner_p = (share_mode_record *)shmops->offset2addr(mode_array[hash_entry]);
  file_prev_p = file_scanner_p;
  while(file_scanner_p)
  {
    if( (file_scanner_p->st_dev == dev) && (file_scanner_p->st_ino == inode) )
    {
      found = True;
      break;
    }
    else
    {
      file_prev_p = file_scanner_p ;
      file_scanner_p = (share_mode_record *)shmops->offset2addr(
                                    file_scanner_p->next_offset);
    }
  }
  
  if(!found)
  {
    DEBUG(5,("get_share_modes no entry for file dev = %d ino = %d\n",
	     dev, inode));
    return (0);
  }
  
  if(file_scanner_p->locking_version != LOCKING_VERSION)
  {
    DEBUG(0,("ERROR: get_share_modes  Deleting old share mode v1 %d dev=%d ino=%d\n", 
	     file_scanner_p->locking_version, dev, inode));
    if(file_prev_p == file_scanner_p)
      mode_array[hash_entry] = file_scanner_p->next_offset;
    else
      file_prev_p->next_offset = file_scanner_p->next_offset;
    shmops->shm_free(shmops->addr2offset(file_scanner_p));
    return (0);
  }

  /* Allocate the old_shares array */
  num_entries = file_scanner_p->num_share_mode_entries;
  if(num_entries)
  {
    *old_shares = share_array = (share_mode_entry *)
                 malloc(num_entries * sizeof(share_mode_entry));
    if(*old_shares == 0)
    {
      DEBUG(0,("get_share_modes: malloc fail!\n"));
      return 0;
    }
  }

  num_entries_copied = 0;
  
  entry_scanner_p = (shm_share_mode_entry*)shmops->offset2addr(
                                           file_scanner_p->share_mode_entries);
  entry_prev_p = entry_scanner_p;
  while(entry_scanner_p)
  {
    int pid = entry_scanner_p->e.pid;

    if (pid && !process_exists(pid))
    {
      /* Delete this share mode entry */
      shm_share_mode_entry *delete_entry_p = entry_scanner_p;

      if(entry_prev_p == entry_scanner_p)
      {
        /* We are at start of list */
        file_scanner_p->share_mode_entries = entry_scanner_p->next_share_mode_entry;
        entry_scanner_p = (shm_share_mode_entry*)shmops->offset2addr(
                                           file_scanner_p->share_mode_entries);
        entry_prev_p = entry_scanner_p;
      }
      else
      {
        entry_prev_p->next_share_mode_entry = entry_scanner_p->next_share_mode_entry;
        entry_scanner_p = (shm_share_mode_entry*)
                           shmops->offset2addr(entry_scanner_p->next_share_mode_entry);
      }
      /* Decrement the number of share mode entries on this share mode record */
      file_scanner_p->num_share_mode_entries -= 1;

      /* PARANOIA TEST */
      if(file_scanner_p->num_share_mode_entries < 0)
      {
        DEBUG(0,("PANIC ERROR: get_share_mode: entries=%d dev=%d ino=%d\n",
		 file_scanner_p->num_share_mode_entries,dev, inode));
        return 0;
      }

      DEBUG(0,("get_share_modes: process %d no longer exists\n", pid));

      shmops->shm_free(shmops->addr2offset(delete_entry_p));
    } 
    else
    {
       /* This is a valid share mode entry and the process that
           created it still exists. Copy it into the output array.
       */
       share_array[num_entries_copied].pid = entry_scanner_p->e.pid;
       share_array[num_entries_copied].share_mode = entry_scanner_p->e.share_mode;
       share_array[num_entries_copied].op_port = entry_scanner_p->e.op_port;
       share_array[num_entries_copied].op_type = entry_scanner_p->e.op_type;
       memcpy(&share_array[num_entries_copied].time, &entry_scanner_p->e.time,
              sizeof(struct timeval));
       num_entries_copied++;
       DEBUG(5,("get_share_modes Read share mode 0x%X pid=%d\n", 
		entry_scanner_p->e.share_mode, entry_scanner_p->e.pid));
       entry_prev_p = entry_scanner_p;
       entry_scanner_p = (shm_share_mode_entry *)
                           shmops->offset2addr(entry_scanner_p->next_share_mode_entry);
    }
  }
  
  /* If no valid share mode entries were found then this record shouldn't exist ! */
  if(num_entries_copied == 0)
  {
    DEBUG(0,("get_share_modes: file with dev %d inode %d empty\n", 
	     dev, inode));
    if(*old_shares)
      free((char *)*old_shares);
    *old_shares = 0;

    if(file_prev_p == file_scanner_p)
      mode_array[hash_entry] = file_scanner_p->next_offset;
    else
      file_prev_p->next_offset = file_scanner_p->next_offset;
    shmops->shm_free(shmops->addr2offset(file_scanner_p));
  }

  DEBUG(5,("get_share_modes: file with dev %d inode %d -> %d entries\n",
	   dev, inode, num_entries_copied));

  return(num_entries_copied);
}  

/*******************************************************************
del the share mode of a file.
********************************************************************/
static void shm_del_share_mode(int token, int fnum)
{
  uint32 dev, inode;
  int *mode_array;
  unsigned int hash_entry;
  share_mode_record *file_scanner_p;
  share_mode_record *file_prev_p;
  shm_share_mode_entry *entry_scanner_p;
  shm_share_mode_entry *entry_prev_p;
  BOOL found = False;
  int pid = getpid();

  dev = Files[fnum].fd_ptr->dev;
  inode = Files[fnum].fd_ptr->inode;

  hash_entry = HASH_ENTRY(dev, inode);

  mode_array = (int *)shmops->offset2addr(shmops->get_userdef_off());
 
  if(mode_array[hash_entry] == 0)
  {  
    DEBUG(0,("PANIC ERROR:del_share_mode hash bucket %d empty\n", 
                  hash_entry));
    return;
  }  
  
  file_scanner_p = (share_mode_record *)shmops->offset2addr(mode_array[hash_entry]);
  file_prev_p = file_scanner_p;

  while(file_scanner_p)
  {
    if( (file_scanner_p->st_dev == dev) && (file_scanner_p->st_ino == inode) )
    {
      found = True;
      break;
    }
    else
    {
      file_prev_p = file_scanner_p ;
      file_scanner_p = (share_mode_record *)
                        shmops->offset2addr(file_scanner_p->next_offset);
    }
  }
    
  if(!found)
  {
     DEBUG(0,("ERROR: del_share_mode no entry for dev %d inode %d\n",
	      dev, inode));
     return;
  }
  
  if(file_scanner_p->locking_version != LOCKING_VERSION)
  {
    DEBUG(0,("ERROR: del_share_modes Deleting old share mode v1 %d dev=%d ino=%d\n",
	     file_scanner_p->locking_version, dev, inode));
    if(file_prev_p == file_scanner_p)
      mode_array[hash_entry] = file_scanner_p->next_offset;
    else
      file_prev_p->next_offset = file_scanner_p->next_offset;
    shmops->shm_free(shmops->addr2offset(file_scanner_p));
    return;
  }

  found = False;
  entry_scanner_p = (shm_share_mode_entry*)shmops->offset2addr(
                                         file_scanner_p->share_mode_entries);
  entry_prev_p = entry_scanner_p;
  while(entry_scanner_p)
  {
    if( (pid == entry_scanner_p->e.pid) && 
          (memcmp(&entry_scanner_p->e.time, 
                 &Files[fnum].open_time,sizeof(struct timeval)) == 0) )
    {
      found = True;
      break;
    }
    else
    {
      entry_prev_p = entry_scanner_p;
      entry_scanner_p = (shm_share_mode_entry *)
                          shmops->offset2addr(entry_scanner_p->next_share_mode_entry);
    }
  } 

  if (found)
  {
    /* Decrement the number of entries in the record. */
    file_scanner_p->num_share_mode_entries -= 1;

    DEBUG(2,("del_share_modes Deleting share mode entry dev=%d ino=%d\n",
              dev, inode));
    if(entry_prev_p == entry_scanner_p)
      /* We are at start of list */
      file_scanner_p->share_mode_entries = entry_scanner_p->next_share_mode_entry;
    else
      entry_prev_p->next_share_mode_entry = entry_scanner_p->next_share_mode_entry;
    shmops->shm_free(shmops->addr2offset(entry_scanner_p));

    /* PARANOIA TEST */
    if(file_scanner_p->num_share_mode_entries < 0)
    {
      DEBUG(0,("PANIC ERROR:del_share_mode num_share_mode_entries=%d\n", 
	       file_scanner_p->num_share_mode_entries));
      return;
    }

    /* If we deleted the last share mode entry then remove the share mode record. */
    if(file_scanner_p->num_share_mode_entries == 0)
    {
      DEBUG(2,("del_share_modes num entries = 0, deleting share_mode dev=%d ino=%d\n", 
	       dev, inode));
      if(file_prev_p == file_scanner_p)
        mode_array[hash_entry] = file_scanner_p->next_offset;
      else
        file_prev_p->next_offset = file_scanner_p->next_offset;
      shmops->shm_free(shmops->addr2offset(file_scanner_p));
    }
  }
  else
  {
    DEBUG(0,("ERROR: del_share_modes No share mode dev=%d ino=%d\n", 
	     dev, inode));
  }
}

/*******************************************************************
set the share mode of a file. Return False on fail, True on success.
********************************************************************/
static BOOL shm_set_share_mode(int token, int fnum, uint16 port, uint16 op_type)
{
  files_struct *fs_p = &Files[fnum];
  int32 dev, inode;
  int *mode_array;
  unsigned int hash_entry;
  share_mode_record *file_scanner_p;
  share_mode_record *file_prev_p;
  shm_share_mode_entry *new_entry_p;
  int new_entry_offset;
  BOOL found = False;

  dev = fs_p->fd_ptr->dev;
  inode = fs_p->fd_ptr->inode;

  hash_entry = HASH_ENTRY(dev, inode);

  mode_array = (int *)shmops->offset2addr(shmops->get_userdef_off());

  file_scanner_p = (share_mode_record *)shmops->offset2addr(mode_array[hash_entry]);
  file_prev_p = file_scanner_p;
  
  while(file_scanner_p)
  {
    if( (file_scanner_p->st_dev == dev) && (file_scanner_p->st_ino == inode) )
    {
      found = True;
      break;
    }
    else
    {
      file_prev_p = file_scanner_p ;
      file_scanner_p = (share_mode_record *)
                         shmops->offset2addr(file_scanner_p->next_offset);
    }
  }
  
  if(!found)
  {
    /* We must create a share_mode_record */
    share_mode_record *new_mode_p = NULL;
    int new_offset = shmops->shm_alloc(sizeof(share_mode_record) +
				   strlen(fs_p->name) + 1);
    if(new_offset == 0) {
	    DEBUG(0,("ERROR:set_share_mode shmops->shm_alloc fail!\n"));
	    return False;
    }
    new_mode_p = shmops->offset2addr(new_offset);
    new_mode_p->locking_version = LOCKING_VERSION;
    new_mode_p->st_dev = dev;
    new_mode_p->st_ino = inode;
    new_mode_p->num_share_mode_entries = 0;
    new_mode_p->share_mode_entries = 0;
    pstrcpy(new_mode_p->file_name, fs_p->name);

    /* Chain onto the start of the hash chain (in the hope we will be used first). */
    new_mode_p->next_offset = mode_array[hash_entry];
    mode_array[hash_entry] = new_offset;

    file_scanner_p = new_mode_p;

    DEBUG(3,("set_share_mode: Created share record for %s (dev %d inode %d)\n", 
	     fs_p->name, dev, inode));
  }
 
  /* Now create the share mode entry */ 
  new_entry_offset = shmops->shm_alloc(sizeof(shm_share_mode_entry));
  if(new_entry_offset == 0) {
	  int delete_offset = mode_array[hash_entry];
	  DEBUG(0,("ERROR:set_share_mode: shmops->shm_alloc fail 1!\n"));
	  /* Unlink the damaged record */
	  mode_array[hash_entry] = file_scanner_p->next_offset;
	  /* And delete it */
	  shmops->shm_free( delete_offset );
	  return False;
  }

  new_entry_p = shmops->offset2addr(new_entry_offset);

  new_entry_p->e.pid = getpid();
  new_entry_p->e.share_mode = fs_p->share_mode;
  new_entry_p->e.op_port = port;
  new_entry_p->e.op_type = op_type;
  memcpy( (char *)&new_entry_p->e.time, (char *)&fs_p->open_time, sizeof(struct timeval));

  /* Chain onto the share_mode_record */
  new_entry_p->next_share_mode_entry = file_scanner_p->share_mode_entries;
  file_scanner_p->share_mode_entries = new_entry_offset;

  /* PARANOIA TEST */
  if(file_scanner_p->num_share_mode_entries < 0)
  {
    DEBUG(0,("PANIC ERROR:set_share_mode num_share_mode_entries=%d\n", 
	     file_scanner_p->num_share_mode_entries));
    return False;
  }

  /* Increment the share_mode_entries counter */
  file_scanner_p->num_share_mode_entries += 1;

  DEBUG(3,("set_share_mode: Created share entry for %s with mode 0x%X pid=%d\n",
	   fs_p->name, fs_p->share_mode, new_entry_p->e.pid));

  return(True);
}

/*******************************************************************
Remove an oplock port and mode entry from a share mode.
********************************************************************/
static BOOL shm_remove_share_oplock(int fnum, int token)
{
  uint32 dev, inode;
  int *mode_array;
  unsigned int hash_entry;
  share_mode_record *file_scanner_p;
  share_mode_record *file_prev_p;
  shm_share_mode_entry *entry_scanner_p;
  shm_share_mode_entry *entry_prev_p;
  BOOL found = False;
  int pid = getpid();

  dev = Files[fnum].fd_ptr->dev;
  inode = Files[fnum].fd_ptr->inode;

  hash_entry = HASH_ENTRY(dev, inode);

  mode_array = (int *)shmops->offset2addr(shmops->get_userdef_off());

  if(mode_array[hash_entry] == 0)
  {
    DEBUG(0,("PANIC ERROR:remove_share_oplock: hash bucket %d empty\n",
                  hash_entry));
    return False;
  } 
    
  file_scanner_p = (share_mode_record *)shmops->offset2addr(mode_array[hash_entry]);
  file_prev_p = file_scanner_p;
    
  while(file_scanner_p)
  { 
    if( (file_scanner_p->st_dev == dev) && (file_scanner_p->st_ino == inode) )
    {
      found = True;
      break;
    }
    else
    {
      file_prev_p = file_scanner_p ;
      file_scanner_p = (share_mode_record *)
                        shmops->offset2addr(file_scanner_p->next_offset);
    }
  } 
   
  if(!found)
  { 
     DEBUG(0,("ERROR:remove_share_oplock: no entry found for dev=%d ino=%d\n", 
	      dev, inode));
     return False;
  } 

  if(file_scanner_p->locking_version != LOCKING_VERSION)
  {
    DEBUG(0,("ERROR: remove_share_oplock: Deleting old share mode v1=%d dev=%d ino=%d\n",
	     file_scanner_p->locking_version, dev, inode));
    if(file_prev_p == file_scanner_p)
      mode_array[hash_entry] = file_scanner_p->next_offset;
    else
      file_prev_p->next_offset = file_scanner_p->next_offset;
    shmops->shm_free(shmops->addr2offset(file_scanner_p));
    return False;
  }

  found = False;
  entry_scanner_p = (shm_share_mode_entry*)shmops->offset2addr(
                                         file_scanner_p->share_mode_entries);
  entry_prev_p = entry_scanner_p;
  while(entry_scanner_p)
  {
    if( (pid == entry_scanner_p->e.pid) && 
        (entry_scanner_p->e.share_mode == Files[fnum].share_mode) &&
        (memcmp(&entry_scanner_p->e.time, 
                &Files[fnum].open_time,sizeof(struct timeval)) == 0) )
    {
      /* Delete the oplock info. */
      entry_scanner_p->e.op_port = 0;
      entry_scanner_p->e.op_type = 0;
      found = True;
      break;
    }
    else
    {
      entry_prev_p = entry_scanner_p;
      entry_scanner_p = (shm_share_mode_entry *)
                          shmops->offset2addr(entry_scanner_p->next_share_mode_entry);
    }
  } 

  if(!found)
  {
    DEBUG(0,("ERROR: remove_share_oplock: No oplock granted. dev=%d ino=%d\n", 
	     dev, inode));
    return False;
  }

  return True;
}


/*******************************************************************
call the specified function on each entry under management by the
share mode system
********************************************************************/
static int shm_share_forall(void (*fn)(share_mode_entry *, char *))
{
	int i, count=0;
	int *mode_array;
	share_mode_record *file_scanner_p;

	mode_array = (int *)shmops->offset2addr(shmops->get_userdef_off());

	for( i = 0; i < shmops->hash_size(); i++) {
		shmops->lock_hash_entry(i);
		if(mode_array[i] == 0)  {
			shmops->unlock_hash_entry(i);
			continue;
		}

		file_scanner_p = (share_mode_record *)shmops->offset2addr(mode_array[i]);
		while((file_scanner_p != 0) && 
		      (file_scanner_p->num_share_mode_entries != 0)) {
			shm_share_mode_entry *entry_scanner_p = 
				(shm_share_mode_entry *)
				shmops->offset2addr(file_scanner_p->share_mode_entries);

			while(entry_scanner_p != 0) {
				
				if (process_exists(entry_scanner_p->e.pid)) {
					fn(&entry_scanner_p->e, 
					   file_scanner_p->file_name);
					count++;
				}

				entry_scanner_p = 
					(shm_share_mode_entry *)
					shmops->offset2addr(
							    entry_scanner_p->next_share_mode_entry);
			} /* end while entry_scanner_p */
			file_scanner_p = (share_mode_record *)
				shmops->offset2addr(file_scanner_p->next_offset);
		} /* end while file_scanner_p */
		shmops->unlock_hash_entry(i);
	} /* end for */

	return count;
}


/*******************************************************************
dump the state of the system
********************************************************************/
static void shm_share_status(FILE *f)
{
	int bytes_free, bytes_used, bytes_overhead, bytes_total;

	shmops->get_usage(&bytes_free, &bytes_used, &bytes_overhead);
	bytes_total = bytes_free + bytes_used + bytes_overhead;

	fprintf(f, "Share mode memory usage (bytes):\n");
	fprintf(f, "   %d(%d%%) free + %d(%d%%) used + %d(%d%%) overhead = %d(100%%) total\n",
		bytes_free, (bytes_free * 100)/bytes_total,
		bytes_used, (bytes_used * 100)/bytes_total,
		bytes_overhead, (bytes_overhead * 100)/bytes_total,
		bytes_total);
}


static struct share_ops share_ops = {
	shm_stop_share_mode_mgmt,
	shm_lock_share_entry,
	shm_unlock_share_entry,
	shm_get_share_modes,
	shm_del_share_mode,
	shm_set_share_mode,
	shm_remove_share_oplock,
	shm_share_forall,
	shm_share_status,
};

/*******************************************************************
  initialize the shared memory for share_mode management 
  ******************************************************************/
struct share_ops *locking_shm_init(int ronly)
{
	read_only = ronly;

#ifdef USE_SYSV_IPC
	shmops = sysv_shm_open(read_only);
	if (shmops) return &share_ops;
#endif

	shmops = smb_shm_open(read_only);
	if (shmops) return &share_ops;

	return NULL;
}

#else
 int locking_shm_dummy_procedure(void)
{return 0;}
#endif



