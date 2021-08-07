/*
  (c) 2021 Chris Royle
    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.

*/

/* Although written from scratch, this code could not have been
   developed without sight of the work published on github 
   at https://github.com/stardot/ArduinoFilestore
   The author of that code's efforts are acknowledged herein.
   In particular, what has been useful has been the insight into
   format of those calls and the necessary replies. Without that,
   this code would have taken significantly longer to create.

   Sadly the identity of the author of that code has not yet
   become apparent to me, but if anybody knows I will gladly 
   acknowledge them by name.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <dirent.h>
#include <regex.h>
#include <sys/xattr.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <sys/syscall.h>
#include <sys/sendfile.h>
#include <ctype.h>

#include "../include/econet-gpio-consumer.h"

// the ] as second character is a special location for that character - it loses its
// special meaning as 'end of character class' so you can match on it.
#define FSREGEX "[]\\*\\#A-Za-z0-9\\+_;:[\\?/\\£\\!\\@\\%\\\\\\^\\{\\}\\+\\~\\,\\=\\<\\>\\|\\-]"

extern int aun_send (struct __econet_packet_udp *, int, short, short, short, short);
#ifdef ECONET_64BIT
extern unsigned int local_seq;
#else
extern unsigned long local_seq;
#endif

short fs_sevenbitbodge; // Whether to use the spare 3 bits in the day byte for extra year information

short fs_open_interlock(int, unsigned char *, unsigned short, unsigned short);
void fs_close_interlock(int, unsigned short, unsigned short);

#define FS_VERSION_STRING "6.0a"

// Implements basic AUN fileserver within the econet bridge

#define ECONET_MAX_FS_SERVERS 4
#define ECONET_MAX_FS_USERS 256
#define ECONET_MAX_FS_DISCS 10 // Don't change this. It won't end well.
#define ECONET_MAX_FS_DIRS 256 // maximum number of active directory handles
#define ECONET_MAX_FS_FILES 512 // Maximum number of active file handles

#define FS_PRIV_SYSTEM 0x80
#define FS_PRIV_LOCKED 0x40
#define FS_PRIV_NOPASSWORDCHANGE 0x20
#define FS_PRIV_USER 0x01
#define FS_PRIV_INVALID 0x00

#define FS_BOOTOPT_OFF 0x00
#define FS_BOOTOPT_LOAD 0x01
#define FS_BOOTOPT_RUN 0x02
#define FS_BOOTOPT_EXEC 0x03

struct {
	unsigned char username[10];
	unsigned char password[6];
	unsigned char fullname[30];
	unsigned char priv;
	unsigned char bootopt;
	unsigned char home[96];
	unsigned char lib[96];
	unsigned char home_disc;
	unsigned char year, month, day, hour, min, sec; // Last login time
	char padding[9]; // Makes up to 256 bytes per user
} users[ECONET_MAX_FS_SERVERS][ECONET_MAX_FS_USERS];

#define FS_MAX_OPEN_FILES 33 // Really 32 because we don't use entry 0

struct {
	unsigned char net, stn;
	unsigned int userid; // Index into users[n][]
	unsigned char root, current, lib; // Handles
	char root_dir[256], current_dir[256], lib_dir[256]; // Paths relative to root
	char root_dir_tail[15], lib_dir_tail[15], current_dir_tail[15]; // Just the last element of path, or $
	unsigned int home_disc, current_disc, lib_disc; // Currently selected disc for each of the three handles
	unsigned char bootopt;
	unsigned char priv;
	struct {
		short handle; // Pointer into fs_files
		unsigned long cursor; // Our pointer into the file
		unsigned short mode; // 1 = read, 2 = openup, 3 = openout
		unsigned char sequence; // Oscillates 0-1-0-1... allows FS to detect retransmissions
		unsigned short pasteof; // Signals when there has already been one attempt to read past EOF and if there's another we need to generate an error
		unsigned short is_dir; // Looks like Acorn systems can OPENIN() a directory so there has to be a single set of handles between dirs & files. So if this is non-zero, the handle element is a pointer into fs_dirs, not fs_files.
		char acornfullpath[1024]; // Full Acorn path, used for calculating relative paths
	} fhandles[FS_MAX_OPEN_FILES];
	struct {
		short handle; // Pointer into fs_dirs
		unsigned long cursor; // ftell() cursor
	} dhandles[FS_MAX_OPEN_FILES];
} active[ECONET_MAX_FS_SERVERS][ECONET_MAX_FS_USERS];

struct {
	unsigned char net; // Network number of this server
	unsigned char stn; // Station number of this server
	unsigned char directory[256]; // Root directory
	unsigned int total_users; // How many entries in users[][]?
#ifdef ECONET_64BIT
	unsigned int seq;
#else
	unsigned long seq;
#endif
	int total_discs;
} fs_stations[ECONET_MAX_FS_SERVERS];

struct {
	unsigned char name[17];
} fs_discs[ECONET_MAX_FS_SERVERS][ECONET_MAX_FS_DISCS];

struct {
	unsigned char name[1024];
	FILE *handle;
	int readers, writers; // Used for locking; when readers = writers = 0 we close the file 
} fs_files[ECONET_MAX_FS_SERVERS][ECONET_MAX_FS_FILES];

struct {
	unsigned char name[1024];
	DIR *handle;
	int readers; // When 0, we close the handle
} fs_dirs[ECONET_MAX_FS_SERVERS][ECONET_MAX_FS_FILES];

struct {
	unsigned char net, stn;
	short handle; // -1 if available
	unsigned char ack_port;
	unsigned char reply_port;
	unsigned char rx_ctrl;
	unsigned long length;
	unsigned long received;
	unsigned short mode; // as in 1 read, 2 updated, 3 write & truncate (I think!)
	unsigned short active_id; // 0 = no user handle because we are doing a fs_save
	unsigned short user_handle; // index into active[server][active_id].fhandles[] so that cursor can be updated
	unsigned long long last_receive; // Time of last receipt so that we can garbage collect	
} fs_bulk_ports[ECONET_MAX_FS_SERVERS][256];

struct objattr {
	unsigned short perm;
	unsigned short owner;
	unsigned long load, exec;
};

#define FS_FTYPE_NOTFOUND 0
#define FS_FTYPE_FILE 1
#define FS_FTYPE_DIR 2
#define FS_FTYPE_SPECIAL 3 // Not sure what I'll use that for, but we'll have it anyhow

#define FS_PERM_H 0x80 // Hidden - doesn't show up in directory list, but can be opened
#define FS_PERM_OTH_W 0x20 // Write by others
#define FS_PERM_OTH_R 0x10 // Read by others
#define FS_PERM_L 0x04 // Locked
#define FS_PERM_OWN_W 0x02 // Write by owner
#define FS_PERM_OWN_R 0x01 // Read by owner

struct path_entry {
	short ftype;
	int owner, parent_owner;
	unsigned char ownername[11];
	unsigned short perm, parent_perm, my_perm;
	unsigned long load, exec, length, internal;
	unsigned char unixpath[1024], unixfname[15], acornname[15];
	unsigned char day, monthyear;
	void *next, *parent;
};

#define FS_PATH_ERR_NODIR 0x01 // Path searched for had a directory that did not exist
#define FS_PATH_ERR_FORMAT 0x02 // Path searched for contained invalid material (e.g. started with a '.')
#define FS_PATH_ERR_NODISC 0x03 // Selected disc does not exist
#define FS_PATH_ERR_TYPE 0x04 // What we found was neither file nor directory (even on following a symlink)
#define FS_PATH_ERR_LENGTH 0x05 // Path provided was too long or too short

struct path {
	unsigned short error; // One of FS_PATH_ERR* - only valid if function returns 0
	short ftype; // ECONET_FTYPE_DIR, ECONET_FTYPE_FILE
	// If ftype == NOTFOUND, the rest of the fields are invalid
	unsigned char discname[30]; // Actually max 10 chars. This is just safety.
	short disc; // Disc number
	unsigned char path[30][11]; // Path elements in order, relative to root
	unsigned char acornname[11]; // Acorn format filename - tail end
	short npath; // Number of entries in path[]. 1 means last entry is [0]
	unsigned char path_from_root[256]; // Path from root directory in Econet format
	int owner; // Owner user ID
	int parent_owner;
	unsigned char ownername[11]; // Readable name of owner
	unsigned short perm; // Permissions for owner & other - ECONET_PERM_... etc.
	unsigned short parent_perm; // If object is not found or is a file, this contains permission on parent dir
	unsigned short my_perm; // This user's access rights to this object - i.e. only bottom 3 bits of perm, adjusted for ownership
	unsigned long load, exec, length;
	unsigned long internal; // System internal name for file. (aka inode number for us)
	struct objattr attr; // Not yet in use generally
	unsigned char unixpath[1024]; // Full unix path from / in the filesystem (done because Econet is case insensitive)
	unsigned char acornfullpath[1024]; // Full acorn path within this server, including disc name
	unsigned char unixfname[15]; // As stored on disc, in case different case to what was requested
	unsigned char day; // day of month last written
	unsigned char monthyear; // Top 4 bits years since 1981; bottom four are month (Not very y2k...)
	struct path_entry *paths, *paths_tail; // pointers to head and tail of a linked like of path_entry structs. These are dynamically malloced by the wildcard normalize function and must be freed by the caller. If FS_FTYPE_NOTFOUND, then both will be NULL.
};
	
regex_t r_pathname, r_wildcard;

int fs_count = 0;

unsigned short fs_quiet = 0;

// Convert our perm storage to Acorn / MDFS format
unsigned char fs_perm_to_acorn(unsigned char fs_perm, unsigned char ftype)
{
	unsigned char r;

	r = fs_perm & FS_PERM_H; // High bit

	if (ftype == FS_FTYPE_DIR)
		r |= 0x20;

	if (fs_perm & FS_PERM_L)
		r |= 0x10;

	r |= ((fs_perm & (FS_PERM_OWN_R | FS_PERM_OWN_W)) << 2);
	r |= ((fs_perm & (FS_PERM_OTH_R | FS_PERM_OTH_W)) >> 4);
	
	//if (!fs_quiet) fprintf (stderr, "Converted perms %02X (ftype %02d) to Acorn %02X\n", fs_perm, ftype, r);
	return r;
	

}

// Convert d/m/y to Acorn 2-byte format
void fs_date_to_two_bytes(unsigned short day, unsigned short month, unsigned short year, unsigned char *monthyear, unsigned char *dday)
{
	unsigned char year_internal;

	*dday = (unsigned char) (day & 0x1f);

	*monthyear = (unsigned char) (month & 0x0f);

	year_internal = year;

	if (year_internal >= 1900) year_internal -=1900;

	year_internal = year  - 81;

	//fprintf (stderr, "7 bit bodge is %s\n", (fs_sevenbitbodge ? "on" : "off"));
	if (!fs_sevenbitbodge)
	{
		year_internal -= 40;
		year_internal = year_internal << 4;
		*monthyear |= (year_internal & 0x0f);
		//fprintf (stderr, "Converted %02d/%02d/%02d to MY=%02X, D=%02X\n", day, month, year, *monthyear, *dday);
	}
	else // use top three bits of day as low three bits of year
	{
		*dday |= ((year_internal & 0x07) << 5);
		*monthyear |= (((year_internal & 0x78) << 1) & 0xf0);
		//fprintf (stderr, "Converted %02d/%02d/%04d to MY=%02X, D=%02X\n", day, month, year, *monthyear, *dday);
	}

}

unsigned short fs_year_from_two_bytes(unsigned char day, unsigned char monthyear)
{

	unsigned short r;

	if (!fs_sevenbitbodge)
		r = ((((monthyear & 0xf0) >> 4) + 81) % 100);
	else
		r = ((( ((monthyear & 0xf0) >> 1) | ((day & 0xe0) >> 5) ) + 81) % 100);

	//fprintf (stderr, "year_from2byte (%02x, %02x) = %02d\n", day, monthyear, r);

	return r;

}

unsigned short fs_month_from_two_bytes(unsigned char day, unsigned char monthyear)
{
	return (monthyear & 0x0f);
}

unsigned short fs_day_from_two_bytes(unsigned char day, unsigned char monthyear)
{
	return (day & 0x1f);
}

// Used with scandir
int fs_alphacasesort(const struct dirent **d1, const struct dirent **d2)
{

	return strcasecmp((*d1)->d_name, (*d2)->d_name);
}

// Often Econet clients send strings which are terminated with 0x0d. This copies them so we don't repeat the routine.
void fs_copy_to_cr(unsigned char *dest, unsigned char *src, unsigned short len)
{
	unsigned short count;

	count = 0;

	while (count < len && *(src+count) != 0x0d)
	{
		*(dest+count) = *(src+count);
		count++;
	}

	*(dest+count) = '\0';	
}

int fs_aun_send(struct __econet_packet_udp *p, int server, int len, unsigned short net, unsigned short stn)
{
	p->p.pad = 0x00;
	p->p.seq = (fs_stations[server].seq += 4);
	return aun_send (p, 8+len, fs_stations[server].net, fs_stations[server].stn, net, stn);
}

unsigned short fs_get_dir_handle(int server, unsigned int active_id, unsigned char *path)
{
	unsigned short count;

	unsigned short found;

	count = 0; found = 0;

	while (!found && count < FS_MAX_OPEN_FILES)
	{
		if (!strcasecmp((const char *) fs_dirs[server][count].name, (const char *) path)) // Already open
		{
			fs_dirs[server][count].readers++;	
			found = 1;
			return count;
		}
		else count++;
	}

	if (!found) // Open the directory
	{
		found = 0;
		count = 0;
		while (!found && count < FS_MAX_OPEN_FILES)
		{
			if (fs_dirs[server][count].handle == NULL)
			{
				found = 1;
				if (!(fs_dirs[server][count].handle = opendir((const char *) path))) // Open failed!
					return -1;
				fs_dirs[server][count].readers = 1;
				return count;	

			}
			else count++;

		}


	}

	return -1;
}

void fs_close_dir_handle(int server, unsigned short handle)
{
	if (!(fs_dirs[server][handle].handle)) // Not open!
		return;

	if (fs_dirs[server][handle].readers > 0)
		fs_dirs[server][handle].readers--;

	if (fs_dirs[server][handle].readers == 0) // Nobody left
	{
		closedir(fs_dirs[server][handle].handle);
		fs_dirs[server][handle].handle = NULL;
	}

	return;

}

// Find a user file channel
// Gives 0 on failure
unsigned short fs_allocate_user_file_channel(int server, unsigned int active_id)
{
	unsigned short count; // f is index into fs_files[server]

	count = 1; // Don't want to feed the user a directory handle 0

	while (active[server][active_id].fhandles[count].handle != -1 && count < FS_MAX_OPEN_FILES)
		count++;

	if (count == FS_MAX_OPEN_FILES) return 0; // No handle available

	active[server][active_id].fhandles[count].is_dir = 0;

	return count;

}

// Deallocate a file handle for a user
void fs_deallocate_user_file_channel(int server, unsigned int active_id, unsigned short channel)
{
	// Do nothing if it's actually a directory handle

	if (active[server][active_id].fhandles[channel].is_dir) return;

	active[server][active_id].fhandles[channel].handle = -1;
	
	return;
}

// Take a unix DIR* handle and find a slot for it in the user's data
unsigned short fs_allocate_user_dir_channel(int server, unsigned int active_id, short d)
{
	unsigned short count;

	count = 1; // Don't want to feed the user a directory handle 0

// All references to dhandles changed to fhandle so there is a single set of user handles

	while (active[server][active_id].fhandles[count].handle != -1 && count < FS_MAX_OPEN_FILES)
		count++;

	if (count == FS_MAX_OPEN_FILES) return -1; // No handle available

	active[server][active_id].fhandles[count].handle = d;
	active[server][active_id].fhandles[count].cursor = 0;
	active[server][active_id].fhandles[count].is_dir = 1;

	return count;

}

// Deallocate a directory handle for a user
void fs_deallocate_user_dir_channel(int server, unsigned int active_id, unsigned short channel)
{

	// If it's not a directory handle, do nothing. ALl refs herein changed from dhandles to fhandles

	if (active[server][active_id].fhandles[channel].is_dir == 0) return;

	if (active[server][active_id].fhandles[channel].handle != -1)
		fs_close_dir_handle(server, active[server][active_id].fhandles[channel].handle);

	active[server][active_id].fhandles[channel].handle = -1;
	
	return;
}


int fs_reply_success(int server, unsigned short reply_port, unsigned short net, unsigned short stn, unsigned short command, unsigned short result)
{

	struct __econet_packet_udp reply;

	reply.p.ptype = ECONET_AUN_DATA;
	reply.p.port = reply_port;
	reply.p.ctrl = 0x80;
	reply.p.pad = 0x00;
	reply.p.seq = (fs_stations[server].seq += 4);
	reply.p.data[0] = command;
	reply.p.data[1] = result;

	return fs_aun_send(&reply, server, 2, net, stn);

}

// Find index into users[server] with net,stn number
int fs_find_userid(int server, unsigned char net, unsigned char stn)
{

	unsigned int index = 0;

	while (index < ECONET_MAX_FS_USERS)
	{
		if (active[server][index].net == net && active[server][index].stn == stn)
			return active[server][index].userid;
	
		index++;
	}

	return -1;	 // userid may be 0

}

// Checks an open directory handle to see if *e exists on a case insensitive basis
// Returns 1 if it exists; otherwise 0
// On a successful return, gives the Unix name in the directory, adjusted for / -> : (Econet -> Unix)
// in *r

int fs_check_dir(DIR *h, char *e,  char *r)
{

	short found;
	struct dirent *d;

	found = 0;

	while ((d = readdir(h)) && !found)
	{
		// Examine, sort out xattr as need be, set parent_owner if we find it...
		// Not the case that the last entry has to be a file because this routine will be used for changing directory too

		if (!strcasecmp((const char *) d->d_name, (const char *) e)) // Match!
		{
			strcpy((char * ) r, d->d_name);
			found = 1;
			break;
		}
		
	}

	return found;

}

void fs_read_xattr(unsigned char *path, struct objattr *r)
{

	unsigned char attrbuf[20];

	if (getxattr((const char *) path, "user.econet_owner", attrbuf, 4) >= 0) // Attribute found
	{
		attrbuf[4] = '\0';
		r->owner = strtoul((const char * ) attrbuf, NULL, 16);
	}
	else	r->owner = 0; // Syst

	if (getxattr((const char *) path, "user.econet_load", attrbuf, 8) >= 0) // Attribute found
	{
		attrbuf[8] = '\0';
		r->load = strtoul((const char * ) attrbuf, NULL, 16);
	}
	else	r->load = 0; 

	if (getxattr((const char *) path, "user.econet_exec", attrbuf, 8) >= 0) // Attribute found
	{
		attrbuf[8] = '\0';
		r->exec = strtoul((const char * ) attrbuf, NULL, 16);
	}
	else	r->exec = 0; 

	if (getxattr((const char *) path, "user.econet_perm", attrbuf, 2) >= 0) // Attribute found
	{
		attrbuf[2] = '\0';
		r->perm = strtoul((const char * ) attrbuf, NULL, 16);
	}
	else	r->perm = FS_PERM_OWN_R | FS_PERM_OWN_W | FS_PERM_OTH_R; 

	return;

}

void fs_write_xattr(unsigned char *path, int owner, short perm, unsigned long load, unsigned long exec)
{

	unsigned char attrbuf[20];

	sprintf ((char * ) attrbuf, "%02x", perm);
	if (setxattr((const char *) path, "user.econet_perm", (const void *) attrbuf, 2, 0)) // Flags = 0 means create if not exist, replace if does
		fprintf (stderr, "   FS: Failed to set permission on %s\n", path);

	sprintf((char * ) attrbuf, "%04x", owner);
	if (setxattr((const char *) path, "user.econet_owner", (const void *) attrbuf, 4, 0))
		fprintf (stderr, "   FS: Failed to set owner on %s\n", path);

	sprintf((char * ) attrbuf, "%08lx", load);
	if (setxattr((const char *) path, "user.econet_load", (const void *) attrbuf, 8, 0))
		fprintf (stderr, "   FS: Failed to set load address on %s\n", path);

	sprintf((char * ) attrbuf, "%08lx", exec);
	if (setxattr((const char *) path, "user.econet_exec", (const void *) attrbuf, 8, 0))
		fprintf (stderr, "   FS: Failed to set exec address on %s: %s\n", path, strerror(errno));

}

// Convert filename from acorn to unix (replace / with :)
void fs_acorn_to_unix(char *string)
{

	unsigned short counter = 0;

	while (*(string+counter) != '\0')
	{
		if (*(string+counter) == '/')
			*(string+counter) = ':';
		counter++;
	}

}

// Convert filename from unix to acorn format (replace : with /)
void fs_unix_to_acorn(char *string)
{

	unsigned short counter = 0;

	while (*(string+counter) != '\0')
	{
		if (*(string+counter) == ':')
			*(string+counter) = '/';
		counter++;
	}

}

// output must be suitably sized - the regex string is quite long!
void fs_wildcard_to_regex(char *input, char *output)
{

	unsigned short counter = 0;
	char internal[1024];

	strcpy(internal, "");

	while (*(input+counter) != '\0')
	{
		switch (*(input+counter))
		{
			case '#': // single character wildcard
				strcat(internal, FSREGEX);
				break;
			case '*': // Multi-character regex
				strcat(internal, FSREGEX);
				strcat(internal, "*");
				break;
			default:
			{
				unsigned char t[2];
				t[0] = *(input+counter);
				t[1] = '\0';
				strcat(internal, t);
			}
			break;

		}

		counter++;

	}
	
	sprintf(output, "^%s$", internal);

}

// Does a regcomp on string into r_wildcard to save the bother of coding the same every time
// Puts the right flags on the call too
int fs_compile_wildcard_regex(char *string)
{
	return regcomp(&r_wildcard, string, REG_EXTENDED | REG_ICASE | REG_NOSUB);
}

// Makes sure we aren't more than 10 characters long,
// does a case insensitive regex match on the r_wildcard regex (which
// the caller must have already provided and compiled)
int fs_scandir_filter(const struct dirent *d)
{
	if ((regexec(&r_wildcard, d->d_name, 0, NULL, 0) == 0) && (strlen(d->d_name) <= 10) && strcasecmp(d->d_name, "lost+found"))
		return 1;
	else	return 0;

}

// Frees a *SCANDIR* list of entries. NOT an fs_wildcard_entries chain.
void fs_free_scandir_list(struct dirent ***list, int n)
{

	struct dirent **l;

	l = *list;

	while (n--)
		free(l[n]);
	free (l);

}

void fs_free_wildcard_list(struct path *p)
{
	struct path_entry *pointer, *pointer_next;

	pointer = p->paths;

	while (pointer != NULL)
	{
		pointer_next = pointer->next;
		free (pointer);
		pointer = pointer_next;
	}	

}

// Wildcard directory search. Assumes that the acorn name provided has not yet been converted so that / needs switching for :
// mallocs a linked chain of struct path_entrys, and puts the address of the head in *head and the tail in *tail
// The calling function MUST free those up on or after return.
// The needle must already be converted from wildcards to regex-compatible text.
int fs_get_wildcard_entries (int server, int userid, char *haystack, char *needle, struct path_entry **head, struct path_entry **tail)
{

	unsigned short results, counter;
	struct path_entry *p, *new_p;
	char needle_wildcard[2048];
	struct dirent **namelist;
	struct stat statbuf;
	struct objattr oa, oa_parent;
	struct tm ct;

	fs_acorn_to_unix(needle);

	fs_wildcard_to_regex(needle, needle_wildcard);


	if (fs_compile_wildcard_regex(needle_wildcard) != 0) // Error
		return -1;

	results = scandir(haystack, &namelist, fs_scandir_filter, fs_alphacasesort);

	if (results == -1) // Error - e.g. not found, or not a directory
		return 0;

	// Convert to a path_entry chain here and assign head & tail.

	counter = 0;
	*head = *tail = p = NULL;

	while (counter < results)
	{
		//fprintf (stderr, "fs_get_wildcard_entries() loop counter %d of %d - %s\n", counter+1, results, namelist[counter]->d_name);

		new_p = malloc(sizeof(struct path_entry));	
		new_p->next = NULL;
		if (p == NULL)
		{
			new_p->parent = NULL;
			*head = new_p;
		}
		else
		{
			new_p->parent = p;
			p->next = new_p;
		}

		*tail = new_p;

		// Read parent information

		fs_read_xattr(p->unixpath, &oa_parent);

		// Fill the struct
		
		strncpy (new_p->unixfname, namelist[counter]->d_name, 10);
		new_p->unixfname[10] = '\0';

		strncpy (new_p->acornname, namelist[counter]->d_name, 10);
		new_p->acornname[10] = '\0';

		fs_unix_to_acorn(new_p->acornname);

		sprintf (new_p->unixpath, "%s/%s", haystack, new_p->unixfname);

		if (stat(new_p->unixpath, &statbuf) != 0) // Error
		{
			fprintf(stderr, "Unable to stat %s\n", p->unixpath);
			free (new_p);
			counter++;
			continue;
		}

		p = new_p; // update p

		fs_read_xattr(p->unixpath, &oa);

		p->load = oa.load;
		p->exec = oa.exec;
		p->owner = oa.owner;
		p->perm = oa.perm;
		p->length = statbuf.st_size;

		p->parent_owner = oa_parent.owner;
		p->parent_perm = oa_parent.perm;

		if (p->owner == userid)
			p->my_perm = (p->perm & ~(FS_PERM_OTH_W | FS_PERM_OTH_R));
		else
			p->my_perm = (p->perm & (FS_PERM_L | FS_PERM_H)) | ((p->perm & (FS_PERM_OTH_W | FS_PERM_OTH_R)) >> 4);

		if (S_ISREG(statbuf.st_mode))
			p->ftype = FS_FTYPE_FILE;
		else if (S_ISDIR(statbuf.st_mode))
			p->ftype = FS_FTYPE_DIR;
		else	p->ftype = FS_FTYPE_SPECIAL;

		if (!(S_ISREG(statbuf.st_mode)))
			p->load = p->exec = 0;
	
		localtime_r(&(statbuf.st_mtime), &ct);

		fs_date_to_two_bytes (ct.tm_mday, ct.tm_mon+1, ct.tm_year, &(p->monthyear), &(p->day));	
		/*
		p->day = ct.tm_mday;
		p->monthyear = (((ct.tm_year - 40 - 81) & 0x0f) << 4) | ((ct.tm_mon+1) & 0x0f); // Top four bits are year since 1981, so we deduct 40 from the actual year so that 2021 = 1981; then we OR-in the month
		*/

		p->internal = statbuf.st_ino;
		strncpy(p->ownername, users[server][p->owner].username, 10);
		p->ownername[10] = '\0';

		counter++;
	}

	if (results > 0) fs_free_scandir_list(&namelist, results);

	return results;
}


// Split a pathname supplied by the user into its components. Always relative to
// root directory of the relevant disc
// Also retrieves attributes etc. and unix filename
// user is an index into active[server][]

// If wildcard = 0, the system will assume no wildcards. Otherwise wildcards enabled.

// We need to amend this to return -1 if it's a bad path so the calling routine can distinguish between no entries and bad pathname

int fs_normalize_path_wildcard(int server, int user, unsigned char *path, short relative_to, struct path *result, unsigned short wildcard)
{

	int ptr = 2;
	regmatch_t matches[20];
	unsigned char adjusted[1048];
	unsigned char path_internal[1024];
	unsigned char unix_segment[20];
	short normalize_debug = 0;
	struct objattr attr;
	int parent_owner = 0;
	short found;

	DIR *dir;
	//struct dirent *d;
	short count;

	result->npath = 0;
	result->paths = result->paths_tail = NULL;

	result->disc = -1; // Rogue so that we can tell if there was a discspec in the path

	if (normalize_debug) fprintf (stderr, "Path provided: '%s'\n", path);

	// Truncate any path provided that has spaces in it
	count = 0; 
	while (count < strlen(path))
	{
		if (path[count] == 0x20) path[count] = '\0';
		count++;
	}

	memset(path_internal, 0, 1024);

	if (*path == ':') // Disc selection
	{

		int count, found = 0;

		// Exclude lost+found!
		if (strcasecmp(path+1, "lost+found") && regexec(&r_pathname, (const char * ) path+1, 1, matches, 0) == 0)
		{
			strncpy((char * ) result->discname, (const char * ) path+1, matches[0].rm_eo - matches[0].rm_so);
			*(result->discname + matches[0].rm_eo - matches[0].rm_so) = '\0';
			//strcpy(adjusted, path+strlen((const char *) result->discname)+2); // +2 because there will be a : at the start, and a '.' at the end of the disc name	
			// Copy back to path
			if (*(path+strlen((const char *) result->discname)+2) == '$') // Can't specify home with disc specifier || *(path+strlen((const char *) result->discname)+2) == '&')
				strcpy ((char * ) path_internal, (const char * ) path + strlen((const char *) result->discname) + 2);
			else // insert a $. on the start
			{
				if (strlen(path) == (strlen((const char *) result->discname)+1)) // Just : and a disc name
					strcpy ((char *) path_internal, "$");
				else // There was more beyond the disc name, but not a $ so insert $.
				{
					strcpy ((char * ) path_internal, "$.");
					strcat (path_internal, path + strlen((const char *) result->discname) + 2);
				}
			}
			ptr = 0; // We have put the residual path at the start of path
		}
		else	{ result->error = FS_PATH_ERR_NODISC; return 0; } // Couldn't recognize disc name - bad path

		
		if ( (*(path + strlen((const char *) result->discname) + 1) != '.') && (*(path + strlen((const char *) result->discname) + 1) != '\0') ) // We had neither a '.' nor end of line after the disc name - probably bad. If end of line, then path_internal will have a $ on the front of it - see above.
		{
			result->error = FS_PATH_ERR_FORMAT;
			return 0; // Must be a '.' after the disc name. Was probably attempt at disc name longer than 10 chars.
		}

		// Now see if we know the disc name in our store...

		count = 0;
		while (count < ECONET_MAX_FS_DISCS && !found)
		{
			if (!strcasecmp((const char *) fs_discs[server][count].name, (const char *) result->discname))
				found = 1;
			else 	count++;
		}

		if (!found)
		{
			result->error = FS_PATH_ERR_NODISC;
			return 0; // Bad path - no such disc
		}

		result->disc = count;
	}
	else if (*path == '.') // Bad path - can't start with a .
	{
		result->error = FS_PATH_ERR_FORMAT;
		return 0;
	}
	else	
	{
		strcpy ((char * ) path_internal, (const char * ) path);
	}

	strcpy ((char * ) adjusted, (const char * ) "");

	if (normalize_debug) 
	{
		if (relative_to > 0)
			fprintf (stderr, "Normalize relative to handle %d, which has full acorn path %s\n", relative_to, active[server][user].fhandles[relative_to].acornfullpath);
		else	
			fprintf (stderr, "Normalize relative to nowhere.\n");
	}

/* OLD RELATIVE ADJUSTMENT CODE 
	if (path_internal[0] == '$')
	{
		if (normalize_debug) fprintf (stderr, "Found $ specifier with %02x as next character\n", path_internal[1]);
		switch (path_internal[1])
		{
			case '.': ptr = 2; break; 
			case 0: ptr = 1; break; // next routine will find an empty path
			default: return 0; break; //Anything else is invalid
		}
	}
	else if (relative_to > 0 && relative_to == active[server][user].root) // Packet give root as starting point
	{
		if (normalize_debug) fprintf (stderr, "Adjusting relating to root\n");
		strcpy((char * ) adjusted, (const char * ) active[server][user].root_dir);
		if (strlen((const char *) active[server][user].root_dir) > 0 && (path_internal[0] != '\0')) strcat(adjusted, ".");
		ptr = 0; // Start at beginning of path provided
	}
	else if (relative_to > 0 && relative_to == active[server][user].lib) // Packet gave lib as starting point
	{
		if (normalize_debug) fprintf (stderr, "Adjusting relating to lib\n");
		strcpy((char * ) adjusted, (const char * ) active[server][user].lib_dir);
		if (strlen((const char *) active[server][user].lib_dir) > 0 && (path_internal[0] != '\0')) strcat(adjusted, ".");
		ptr = 0;
	}
	else // Covers no :DISCPATH, no $, no & and relative_to is not lib orroot 
	{
		if (normalize_debug) fprintf (stderr, "Adjusting relative to current (or $ if change of disc)\n");
		if (result->disc == -1) // Relative path when no disc specified
		{
			strcpy((char * ) adjusted, (const char * ) active[server][user].current_dir); // current path
			if (active[server][user].current_dir[0] != '\0' && path_internal[0] != '\0') strcat(adjusted, "."); // Only add . if not empty path
			ptr = 0; // Reset
		}
		// IF there is a disc found, then the path is taken to be realtive to $ anyway
	}

	strcat (adjusted, path_internal + ptr);

*/

	// New relative adjustment code

	if (path_internal[0] == '$') // Absolute path given
	{
		if (normalize_debug) fprintf (stderr, "Found $ specifier with %02x as next character\n", path_internal[1]);
		switch (path_internal[1])
		{
			case '.': ptr = 2; break; 
			case 0: ptr = 1; break; // next routine will find an empty path
			default: result->error = FS_PATH_ERR_FORMAT; return 0; break; //Anything else is invalid
		}
		// Set up 'adjusted' accordingly
		strcpy(adjusted, path_internal + ptr);
	}
	else // relative path given - so give it relative to the relevant handle
	{
		unsigned short fp_ptr = 0;

		if (relative_to < 1) // Relative to nowhere
			strcpy(adjusted, "");
		else
		{
			while (active[server][user].fhandles[relative_to].acornfullpath[fp_ptr] != '.') fp_ptr++;
			// Now at end of disc name
			// Skip the '.$'
			fp_ptr += 2;
			if (active[server][user].fhandles[relative_to].acornfullpath[fp_ptr] == '.') // Path longer than just :DISC.$
				fp_ptr++;
	
			if (fp_ptr < strlen(active[server][user].fhandles[relative_to].acornfullpath))
			{
				sprintf(adjusted, "%s", active[server][user].fhandles[relative_to].acornfullpath + fp_ptr);
				if (strlen(path_internal) > 0) strcat(adjusted, ".");
			}
			else	strcpy(adjusted, "");
		}

		strcat(adjusted, path_internal);

	}
	
	if (result->disc == -1)
	{
		result->disc = active[server][user].current_disc; // Replace the rogue if we are not selecting a specific disc
		strcpy ((char * ) result->discname, (const char * ) fs_discs[server][result->disc].name);
		if (normalize_debug) fprintf (stderr, "No disc specified, choosing current disc: %d - %s\n", active[server][user].current_disc, fs_discs[server][result->disc].name);
	}

	if (normalize_debug) fprintf (stderr, "disc selected = %d, %s\n", result->disc, (result->disc != -1) ? (char *) fs_discs[server][result->disc].name : (char *) "");
	if (normalize_debug) fprintf (stderr, "path_internal = %s (len %d)\n", path_internal, (int) strlen(path_internal));

	sprintf (result->acornfullpath, ":%s.$", fs_discs[server][result->disc].name);

	if (normalize_debug) fprintf (stderr, "Adjusted = %s / ptr = %d / path_internal = %s\n", adjusted, ptr, path_internal);

	strcpy ((char * ) result->path_from_root, (const char * ) adjusted);

	if (normalize_debug) fprintf (stderr, "Adjusted = %s\n", adjusted);

	ptr = 0;

	while (result->npath < 30 && ptr < strlen((const char *) adjusted))
	{

		if ((*(adjusted + ptr) == '^'))
		{
			if (result->npath > 0) result->npath--;
			ptr++;
			if (*(adjusted + ptr) == '.') ptr++; // Skip any . that may be there
		}
		else
		{
			if (regexec(&r_pathname, adjusted + ptr, 1, matches, 0) == 0)
			{
				strncpy((char * ) result->path[result->npath], (const char * ) adjusted + ptr, matches[0].rm_eo - matches[0].rm_so);
				*(result->path[result->npath++] + matches[0].rm_eo - matches[0].rm_so) = '\0';
				ptr += (matches[0].rm_eo - matches[0].rm_so);
			}
			else
			{
				result->error = FS_PATH_ERR_FORMAT;
				return 0; // bad path	
			}
	
			if (ptr != strlen((const char *) adjusted) && *(adjusted + ptr) != '.') // Bad path - must have a dot next, otherwise the path element must be more than ten characters
			{
				result->error = FS_PATH_ERR_FORMAT;
				return 0;
			}
			else if (ptr != strlen((const char *) adjusted) && strlen((const char *) adjusted) == (ptr + 1)) // the '.' was at the end
			{
				result->error = FS_PATH_ERR_FORMAT;
				return 0;
			}
			else 	ptr++; // Move to start of next portion of path
		}

	
	}

	if (ptr < strlen((const char *) adjusted))
	{
		result->error = FS_PATH_ERR_LENGTH;
		return 0; // Path too long!
	}

	/* See if the file exists, in a case insensitive manner, figure out its Unix path, and load its attributes.
	   If no attributes, or some of them are missing, fill them in with appropriate defaults if the file exists */

	/* First build the unix path */

	sprintf (result->unixpath, "%s/%1d%s", fs_stations[server].directory, result->disc, fs_discs[server][result->disc].name);

	if (normalize_debug) fprintf (stderr, "Unix dir: %s\n", result->unixpath);
	if (normalize_debug) fprintf (stderr, "npath = %d\n", result->npath);

	// Iterate through each directory looking for the next part of the path in a case insensitive matter, and if any of them lack extended attributes then add them in as we go (if the thing exists!)
	// Also do the conversion from '/' in an Acorn path to ':' in a unix filename ...

	count = 0;

	// Collect root directory info
	{
		struct stat s;
		struct tm t;
		//int owner;
		char attrbuf[20];

		result->ftype = FS_FTYPE_DIR;
		
		sprintf(result->acornname, "%-10s", "$");

		strcpy((char * ) result->unixfname, (const char * ) "");	 // Root dir - no name
		result->internal = s.st_ino; // Internal name = Inode number
		result->length = 0; // Probably wrong

		// Next, see if we have xattr and, if not, populate them. We do this for all paths along the way

		result->owner = 0; // Always SYST if root directory not owned

		if (getxattr(result->unixpath, "user.econet_load", &attrbuf, 8) >= 0) // Load attribute exists
		{
			attrbuf[8] = '\0';
			result->load = strtoul(attrbuf, NULL, 16);
		}
		else	result->load = 0;

		if (getxattr(result->unixpath, "user.econet_exec", &attrbuf, 8) >= 0) // Exec attribute exists
		{
			attrbuf[8] = '\0';
			result->exec = strtoul(attrbuf, NULL, 16);
		}
		else	result->exec = 0;

		if (getxattr(result->unixpath, "user.econet_perm", &attrbuf, 2) >= 0) // Perm attribut exists
		{
			attrbuf[2] = '\0';
			result->perm = (unsigned short) strtoul(attrbuf, NULL, 16);
		}
		else	result->perm = FS_PERM_OWN_W | FS_PERM_OWN_R; // Default permissions

		fs_write_xattr(result->unixpath, result->owner, result->perm, result->load, result->exec);

		stat(result->unixpath, &s);
		localtime_r(&(s.st_mtime), &t);

		fs_date_to_two_bytes(t.tm_mday, t.tm_mon+1, t.tm_year, &(result->day), &(result->monthyear));

		//result->day = t.tm_mday;
		
		//result->monthyear = (((t.tm_year - 40 - 81) & 0x0f) << 4) | ((t.tm_mon+1) & 0x0f); // Top four bits are year since 1981, so we deduct 40 from the actual year so that 2021 = 1981; then we OR-in the month
	}

	if (wildcard)
	{
		int num_entries;
		unsigned short count = 0;

		char acorn_path[100];
		struct path_entry *p; // Pointer for debug

		if (normalize_debug) fprintf (stderr, "Processing wildcard path with %d elements\n", result->npath);

		// Re-set path_from_root bceause we'll need to update it with the real acorn names
		strcpy(result->path_from_root, "");

		while (result->npath > 0 && (count < result->npath))
		{

			strcpy(acorn_path, result->path[count]); // Preserve result->path[count] as is, otherwise fs_get_wildcard_entries will convert it to unix, which we don't want
			if (normalize_debug) fprintf(stderr, "Processing path element %d - %s (Acorn: %s) in directory %s\n", count, result->path[count], acorn_path, result->unixpath);

			num_entries = fs_get_wildcard_entries(server, active[server][user].userid, result->unixpath, // Current search dir
					acorn_path, // Current segment in Acorn format (which the function will convert)
					&(result->paths), &(result->paths_tail));

			if (normalize_debug)
			{
				fprintf (stderr, "Wildcard search returned %d entries (result->paths = %8p):\n", num_entries, result->paths);
				p = result->paths;
				while (p != NULL)
				{
					fprintf(stderr, "Type %02x Owner %04x Parent owner %04x Owner %10s Perm %02x Parent Perm %02x My Perm %02x Load %08lX Exec %08lX Length %08lX Int name %06lX Unixpath %s Unix fname %s Acorn Name %s Date %02d/%02d/%02d\n",
						p->ftype, p->owner, p->parent_owner, p->ownername,
						p->perm, p->parent_perm, p->my_perm,
						p->load, p->exec, p->length, p->internal,
						p->unixpath, p->unixfname, p->acornname,
						fs_day_from_two_bytes(p->day, p->monthyear),
						fs_month_from_two_bytes(p->day, p->monthyear),
						fs_year_from_two_bytes(p->day, p->monthyear));
						//p->day, p->monthyear & 0x0f, ((!fs_sevenbitbodge) ? (p->monthyear & 0xf0) >> 4) + 81 : (((((p->monthyear & 0xf0) << 1) | ((p->day & 0xe0) >> 5))+81) % 100));
					p = p->next;
				}
			}

			found = (num_entries > 0 ? 1 : 0);

			// If not on last leg, add first entry to path from root
			if (found && (count != result->npath-1))
			{
				if (strlen(result->path_from_root) != 0)
					strcat(result->path_from_root, ".");
				strcat(result->path_from_root, result->paths[0].acornname);
			}

			// Wildcard calls will need to add each successive acornname to the path_from_root for each entry separately - so on the final path element, we don't put it on so that the caller can do that

			if (found == 0) // Didn't find anything
			{
				result->ftype = FS_FTYPE_NOTFOUND;
				// If we are on the last segment and the filename does not contain wildcards, we return 1 to indicate that what was 
				// searched for wasn't there so that it can be written to. Obviously if it did contain wildcards then it can't be so we
				// return 0

				if (normalize_debug) fprintf (stderr, "Work out whether to return 1 or 0 when nothing found: count = %d, result->npath-1=%d, search for wildcards is %s\n", count, result->npath-1, (strchr(result->path[count], '*') == NULL && strchr(result->path[count], '#') == NULL) ? "in vain" : "successful");
				if ((count == result->npath-1)  
					// && ((strchr(result->path[count], '*') == NULL) && (strchr(result->path[count], '#') == NULL))
				) // Only give a hard fail if we are not in last path segment
					return 1;

				result->error = FS_PATH_ERR_NODIR;
				return 0; // If not on last segment, this is a hard fail.
			}
				
			// Always copy the first entry into the main struction because we always want it.
			// Unless on last segment (when we want to leave all the path entries available to be freed by the caller)
			// we free them up here.
			//

			// So there's at least one entry, and it should be at *paths
			//
			//

		 	strncpy(result->ownername, result->paths->ownername, 10);
			result->ownername[10] = '\0';

			result->ftype = result->paths->ftype;
			result->parent_owner = result->paths->parent_owner;
			result->owner = result->paths->owner;
			result->perm = result->paths->perm;
			result->parent_perm = result->paths->parent_perm;
			result->my_perm = result->paths->my_perm;
			result->load = result->paths->load;
			result->exec = result->paths->exec;
			result->length = result->paths->length;
			result->internal = result->paths->internal;
			strncpy (result->acornname, result->paths->acornname, 10);
			result->acornname[10] = '\0';

			if (count < result->npath-1) // Add path to acornfullpath. When in wildcard mode, the caller is expected to add whichever element of paths[] they want to the acornpath to get the full path.
			{
				strcat(result->acornfullpath, ".");
				strcat(result->acornfullpath, result->paths->acornname);
			}

			strcpy (result->unixpath, result->paths->unixpath); // Always copy first entry to unixpath - means that our next npath entry will look in the first thing we found on the last wildcard search. That means, e.g. :ECONET.$.A*.WOMBAT.DR* will match the first thing in $ beginning 'A'.

			strncpy (result->unixfname, result->paths->unixfname, 10);
			result->unixfname[10] = '\0';

			result->day = result->paths->day;
			result->monthyear = result->paths->monthyear;

			if (count != result->npath-1) // Not last segment - free up all the path_entries because we'll be junking them.
				fs_free_wildcard_list(result);

			count++;
		}

		if (normalize_debug) fprintf (stderr, "Returning full acorn path (wildcard - last path element to be added by caller) %s\n", result->acornfullpath);

		return 1;
	}

	// This is the non-wildcard code
	//
	while ((result->npath > 0) && count < result->npath)
	{
		char path_segment[20]; // used to store the converted name (/ -> :)
		struct stat s;
		// OLD char attrbuf[20];
		unsigned short r_counter;
		unsigned short owner, perm;

		found = 0;

		if (normalize_debug) fprintf (stderr, "Examining %s\n", result->unixpath);

		// Convert pathname so that / -> :

		r_counter = 0; 

		while (result->path[count][r_counter] != '\0' && r_counter < 10)
		{
			if (result->path[count][r_counter] == '/')
				path_segment[r_counter] = ':';
			else	path_segment[r_counter] = result->path[count][r_counter];
			r_counter++;
		}
		path_segment[r_counter] = '\0';

// Begin old non-wildcard code
		
		dir = opendir(result->unixpath);

		if (!dir)
		{
			// Not found
			result->ftype = FS_FTYPE_NOTFOUND;
			return 1;
		}

		// if we are looking for last element in path (i.e. result->unixpath currently contains parent directory name)

		if (normalize_debug) fprintf (stderr, "Calling fs_check_dir(..., %s, ...)\n", path_segment);

		// If path_segment is found in dir, then it puts the unix name for that file in unix_segment
		found = fs_check_dir (dir, path_segment, unix_segment);

		closedir(dir);

		// Obtain permissions on dir - see if we can read it

		fs_read_xattr(result->unixpath, &attr);
		owner = attr.owner;
		perm = attr.perm;

		if (count == result->npath - 1) // Last segment
			result->parent_perm = perm;


		if (!	( 
				(active[server][user].priv & FS_PRIV_SYSTEM)
			||	(active[server][user].userid == owner) // Owner can always read own directory irrespective of permissions(!)
			||	(perm & FS_PERM_OTH_R) // Others can read the directory
			)
			&& !found) 
		{
			if (normalize_debug) fprintf (stderr, "This user cannot read dir %s\n", result->unixpath);
			result->ftype = FS_FTYPE_NOTFOUND;
			return 1;
		}
	
		if (!found) // Didn't find any dir entry
		{
			result->ftype = FS_FTYPE_NOTFOUND;
			if (count == (result->npath - 1)) // Not found on last leg - return 1 so that we know it's safe to save there if we have permission
			{
			
				unsigned short r_counter = 0;
				char unix_segment[12];
				while (result->path[count][r_counter] != '\0' && r_counter < 10)
				{
					if (result->path[count][r_counter] == '/')
						unix_segment[r_counter] = ':';
					else	unix_segment[r_counter] = result->path[count][r_counter];
					r_counter++;
				}
				unix_segment[r_counter] = '\0';
				strcat(result->unixpath, "/");
				strcat(result->unixpath, unix_segment); // Add these on a last leg not found so that the calling routine can open the file to write if it wants
				result->parent_owner = parent_owner; // Otherwise this doesn't get properly updated
				return 1;
			}
			else	
			{
				result->error = FS_PATH_ERR_NODIR;
				return 0; // Fatal not found
			}
		}

		if (normalize_debug) fprintf (stderr, "Found path segment %s in unix world = %s\n", path_segment, unix_segment);
		strcat(result->unixpath, "/");
		strcat(result->unixpath, unix_segment);

		// Add it to full acorn path
		strcat(result->acornfullpath, ".");
		strcat(result->acornfullpath, path_segment);

		if (normalize_debug) fprintf (stderr, "Attempting to stat %s\n", result->unixpath);

		if (!stat(result->unixpath, &s)) // Successful stat
		{

			//int owner;
			char dirname[1024];

			if (!S_ISDIR(s.st_mode) && (count < (result->npath - 1))) // stat() follows symlinks so the first bit works across links; the second condition is because we only insist on directories for that part of the path except the last element, which might legitimately be FILE or DIR
			{
				result->ftype = FS_FTYPE_NOTFOUND; // Because something we encountered before end of path could not be a directory
				return 1;
			}

			if ((S_ISDIR(s.st_mode) == 0) && (S_ISREG(s.st_mode) == 0)) // Soemthing is wrong
			{
				result->error = FS_PATH_ERR_TYPE;
				return 0; // Should either be file or directory - not block device etc.
			}

			// Next, set internal name from inode number

			result->internal = s.st_ino; // Internal name = Inode number

			// Next, see if we have xattr and, if not, populate them. We do this for all paths along the way

			strcpy ((char * ) dirname, (const char * ) result->unixpath);
			// Need to add / for setxattr
			if (S_ISDIR(s.st_mode))	strcat(dirname, "/");

			fs_read_xattr(dirname, &attr);
			result->owner = attr.owner;
			result->load = attr.load;
			result->exec = attr.exec;
			result->perm = attr.perm;
			
			result->attr.owner = attr.owner;
			result->attr.load = attr.load;
			result->attr.exec = attr.exec;
			result->attr.perm = attr.perm;

			fs_write_xattr(dirname, result->owner, result->perm, result->load, result->exec);

			result->parent_owner = parent_owner;

			parent_owner = result->owner; // Ready for next loop

			if (normalize_debug) fprintf (stderr, "Setting parent_owner = %04x, this object owned by %04x\n", result->parent_owner, result->owner);

			// Are we on the last entry? If so, this is the leaf we're looking for

			if (count == (result->npath - 1))
			{
				struct tm t;

				if (S_ISDIR(s.st_mode))
				{
					result->ftype = FS_FTYPE_DIR;
					result->load = result->exec = 0;	
					result->length = 0; // This might be wrong
				}
				else // Assume file
				{
					result->ftype = FS_FTYPE_FILE;
					result->length = s.st_size;
				}

				// Modification date

				localtime_r(&(s.st_mtime), &t);

				fs_date_to_two_bytes(t.tm_mday, t.tm_mon+1, t.tm_year, &(result->monthyear), &(result->day));
				//result->day = t.tm_mday;
				//result->monthyear = (((t.tm_year - 81 - 40) & 0x0f) << 4) | ((t.tm_mon+1) & 0x0f); // Top four bits are year since 1981, so we deduct 40 from the actual year so that 2021 = 1981; then we OR-in the month
				
				if (active[server][user].priv & FS_PRIV_SYSTEM)
					result->my_perm = 0xff;
				else if (active[server][user].userid != result->owner)
					result->my_perm = (result->perm & FS_PERM_L) | ((result->perm & (FS_PERM_OTH_W | FS_PERM_OTH_R)) >> 4);
				else	result->my_perm = (result->perm & 0x0f);

				strcpy((char * ) result->unixfname, (const char * ) unix_segment);
	
			}
			
			strcpy((char * ) result->unixfname, (const char * ) unix_segment);
			strncpy(result->acornname, unix_segment, 10);
			result->acornname[10] = '\0';
			fs_unix_to_acorn(result->acornname);

		}
		else	return 0; // Something wrong - that should have existed

		count++;

	}
	
	if (normalize_debug) fprintf (stderr, "Returning full acorn path (non-wildcard) %s\n", result->acornfullpath);

	strncpy((char * ) result->ownername, (const char * ) users[server][result->owner].username, 10); // Populate readable owner name
	result->ownername[10] = '\0';

	return 1; // Success

}

// The old format, non-wildcard function, for backward compat
// Will ultimately need modifying to copy the first entry in the found list into the 
// path structure and then free all the path entries that have been found.
int fs_normalize_path(int server, int user, unsigned char *path, short relative_to, struct path *result)
{
	return fs_normalize_path_wildcard(server, user, path, relative_to, result, 0);
}

void fs_write_user(int server, int user, unsigned char *d) // Writes the 256 bytes at d to the user's record in the relevant password file
{

	char pwfile[1024];
	FILE *h;

	sprintf (pwfile, "%s/Passwords", fs_stations[server].directory);

	if ((h = fopen(pwfile, "r+")))
	{
		if (fseek(h, (256 * user), SEEK_SET))
		{
			if (!fs_quiet) fprintf (stderr, "   FS: Attempt to write beyond end of user file\n");
		}
		else
			fwrite(d, 256, 1, h);

		fclose(h);
	}

}

int fs_initialize(unsigned char net, unsigned char stn, char *serverparam)
{
	
	DIR *d;
	struct dirent *entry;

	int old_fs_count = fs_count;
	
	FILE *passwd;
	char passwordfile[280];
	int length;
	int portcount;
	char regex[256];


// Seven bit bodge test harness

/*
	{
		unsigned char monthyear, day;

		fs_date_to_two_bytes(5, 8, 2021, &monthyear, &day);

		fprintf (stderr, "fs_date_to_two_bytes(5/8/2021) gave MY=%02X, D=%02X\n", monthyear, day);

	}

*/

// WILDCARD TEST HARNESS

/*
	char temp1[15], temp2[2048];
	struct dirent **namelist;
	int sr;

	strcpy(temp1, "FF12/3");
	fprintf(stderr, "   FS: temp1 = %s\n", temp1);
	fs_unix_to_acorn(temp1);
	fprintf(stderr, "   FS: fs_unix_to_acorn(temp1) = %s\n", temp1);
	fs_acorn_to_unix(temp1);
	fprintf(stderr, "   FS: fs_acorn_to_unix(temp1) = %s\n", temp1);
	
	strcpy(temp1, "#e*");
	fprintf(stderr, "   FS: Wildcard test = %s\n", temp1);

	fs_wildcard_to_regex(temp1, temp2);
	fprintf(stderr, "   FS: Wildcard regex = %s\n", temp2);

	fprintf(stderr, "   FS: Regex compile returned %d\n", fs_compile_wildcard_regex(temp2));
	sr = scandir("/econet/0ECONET/CHRIS", &namelist, fs_scandir_filter, fs_alphacasesort);
	
	regfree(&r_wildcard);

	if (sr == -1) fprintf(stderr, "   FS: scandir() test failed.\n");
	else while (sr--)
	{
		fprintf(stderr, "   FS: File index %d = %s\n", sr, namelist[sr]->d_name);
		free(namelist[sr]);	
	}
	free(namelist);
*/
	
// END OF WILDCARD TEST HARNESS

	if (!fs_quiet) fprintf (stderr, "   FS: Attempting to initialize server %d on %d.%d at directory %s\n", fs_count, net, stn, serverparam);

	sprintf(regex, "^(%s{1,10})", FSREGEX);

	//if (regcomp(&r_pathname, "^([A-Za-z0-9\\+_;\\?/\\£\\!\\@\\%\\\\\\^\\{\\}\\+\\~\\,\\=\\<\\>\\|\\-]{1,10})", REG_EXTENDED) != 0)
	if (regcomp(&r_pathname, regex, REG_EXTENDED) != 0)
	{
		fprintf(stderr, "Unable to compile regex for file and directory names.\n");
		exit (EXIT_FAILURE);
	}

	d = opendir(serverparam);

	if (!d)
	{
		fprintf (stderr, "It didn't...\n");
		if (!fs_quiet) fprintf (stderr, "   FS: Unable to open root directory %s\n", serverparam);
	}
	else
	{

		strncpy ((char * ) fs_stations[fs_count].directory, (const char * ) serverparam, 1023);
		fs_stations[fs_count].directory[1024] = (char) 0; // Just in case
		fs_stations[fs_count].net = net;
		fs_stations[fs_count].stn = stn;
		fs_stations[fs_count].seq = 0x4000;

		// Clear state
		/*
		fprintf (stderr, "FS doing memset(%8p, 0, %d)\n", active[fs_count], sizeof(active)/ECONET_MAX_FS_SERVERS);
		fprintf (stderr, "FS doing memset(%8p, 0, %d)\n", fs_discs[fs_count], sizeof(fs_discs)/ECONET_MAX_FS_SERVERS);
		fprintf (stderr, "FS doing memset(%8p, 0, %d)\n", fs_files[fs_count], sizeof(fs_files)/ECONET_MAX_FS_SERVERS);
		fprintf (stderr, "FS doing memset(%8p, 0, %d)\n", fs_dirs[fs_count], sizeof(fs_dirs)/ECONET_MAX_FS_SERVERS);
		fprintf (stderr, "FS bulk ports array at %8p\n", fs_bulk_ports[fs_count]);
		*/
		memset(active[fs_count], 0, sizeof(active)/ECONET_MAX_FS_SERVERS);
		memset(fs_discs[fs_count], 0, sizeof(fs_discs)/ECONET_MAX_FS_SERVERS);
		memset(fs_files[fs_count], 0, sizeof(fs_files)/ECONET_MAX_FS_SERVERS);
		memset(fs_dirs[fs_count], 0, sizeof(fs_dirs)/ECONET_MAX_FS_SERVERS);

		for (length = 0; length < ECONET_MAX_FS_DISCS; length++) // used temporarily as counter
		{
			sprintf (fs_discs[fs_count][length].name, "%29s", "");
			fs_discs[fs_count][length].name[0] = '\0';
		}
	

		sprintf(passwordfile, "%s/Passwords", fs_stations[fs_count].directory);
	
		passwd = fopen(passwordfile, "r+");
		
		if (!passwd)
		{
			if (!fs_quiet) fprintf(stderr, "   FS: No password file - initializing %s with SYST\n", passwordfile);
			sprintf (users[fs_count][0].username, "%-10s", "SYST");
			sprintf (users[fs_count][0].password, "%-6s", "");
			sprintf (users[fs_count][0].fullname, "%-30s", "System User"); 
			users[fs_count][0].priv = FS_PRIV_SYSTEM;
			users[fs_count][0].bootopt = 0;
			sprintf (users[fs_count][0].home, "%-96s", "$");
			sprintf (users[fs_count][0].lib, "%-96s", "$");
			users[fs_count][0].home_disc = 0;
			users[fs_count][0].year = users[fs_count][0].month = users[fs_count][0].day = users[fs_count][0].hour = users[fs_count][0].min = users[fs_count][0].sec = 0; // Last login time
			if ((passwd = fopen(passwordfile, "w+")))
				fwrite(&(users[fs_count]), 256, 1, passwd);
			else if (!fs_quiet) fprintf(stderr, "   FS: Unable to write password file at %s - not initializing\n", passwordfile);
		}

		if (passwd) // Successful file open somewhere along the line
		{
			fseek (passwd, 0, SEEK_END);
			length = ftell(passwd); // Get file size
			rewind(passwd);
	
			if ((length % 256) != 0)
			{
				if (!fs_quiet) fprintf (stderr, "   FS: Password file not a multiple of 256 bytes!\n");
			}
			else if ((length > (256 * ECONET_MAX_FS_USERS)))
			{
				if (!fs_quiet) fprintf (stderr, "   FS: Password file too long!\n");
			}
			else	
			{
				int discs_found = 0;
	
				if (!fs_quiet) fprintf (stderr, "   FS: Password file read - %d user(s)\n", (length / 256));
				fread (&(users[fs_count]), 256, (length / 256), passwd);
				fs_stations[fs_count].total_users = (length / 256);
				fs_stations[fs_count].total_discs = 0;
		
				// Now load up the discs. These are named 0XXX, 1XXX ... FXXXX for discs 0-15
				while ((entry = readdir(d)) && discs_found < ECONET_MAX_FS_DISCS)
				{
					if (((entry->d_name[0] >= '0' && entry->d_name[0] <= '9') || (entry->d_name[0] >= 'A' && entry->d_name[0] <= 'F')) && (entry->d_type == DT_DIR) && (strlen((const char *) entry->d_name) <= 17)) // Found a disc. Length 17 = index character + 16 name; we ignore directories which are longer than that because the disc name will be too long
					{
						int index;
						short count;
						
						index = (int) (entry->d_name[0] - '0');
						if (index > 9) index -= ('A' - '9' - 1);
	
						count = 0;
						while (count < 30 && (entry->d_name[count+1] != 0))
						{
							fs_discs[fs_count][index].name[count] = entry->d_name[1+count];
							count++;
						}
						fs_discs[fs_count][index].name[count] = 0;
					
						if (!fs_quiet) fprintf (stderr, "   FS: Initialized disc name %s (%d)\n", fs_discs[fs_count][index].name, index);
						discs_found++;
	
					}
				}
				
				closedir(d);
				
				for (portcount = 0; portcount < 256; portcount++)
					fs_bulk_ports[fs_count][portcount].handle = -1; 
		
				if (discs_found > 0)
					fs_count++; // Only now do we increment the counter, when everything's worked
				else if (!fs_quiet) fprintf (stderr, "   FS: Server %d - failed to find any discs!\n", fs_count);
			}
			fclose(passwd);
	
			//if (!fs_quiet)
				//fprintf(stderr, "   FS: users = %8p, active = %8p, fs_stations = %8p, fs_discs = %8p, fs_files = %8p, fs_dirs = %8p, fs_bulk_ports = %8p\n",
					//users[fs_count], active[fs_count], fs_stations, fs_discs[fs_count], fs_files[fs_count], fs_dirs[fs_count], fs_bulk_ports[fs_count]);
		}
		
	}
	
	if (fs_count == old_fs_count) // We didn't initialize
		return -1;
	else	
	{

		/* Wildcard test harness on real data
		int result;
		struct path p;

		result = fs_normalize_path_wildcard(old_fs_count, 0, ":ECONET.$.R*.A.*mc*", -1, &p, 1);

		if (result)
		{
			struct path_entry *e;

			e = p.paths;

			while (e != NULL)
			{
					fprintf(stderr, "Type %02x Owner %04x Parent owner %04x Owner %10s Perm %02x Parent Perm %02x My Perm %02x Load %08lX Exec %08lX Length %08lX Int name %06lX Unixpath %s Unix fname %s Acorn Name %s Date %02d/%02d/%02d\n",
						e->ftype, e->owner, e->parent_owner, e->ownername,
						e->perm, e->parent_perm, e->my_perm,
						e->load, e->exec, e->length, e->internal,
						e->unixpath, e->unixfname, e->acornname,
						e->day, e->monthyear & 0x0f, ((e->monthyear & 0xf0) >> 4) + 81);
					e = e->next;

			}

			fs_free_wildcard_list(&p);
		}

		// Test to see if a non-existent filename gives us a 1 return but no path entries, so we know it's writable.
		// We should get a 0 if the tail of the path entry has any wildcards in it
		//

		result = fs_normalize_path_wildcard(old_fs_count, 0, ":ECONET.$.R*.WOBBLE", -1, &p, 1); // Should give us 1 & FS_FTYPE_NOTFOUND

		fprintf (stderr, "Normalize :ECONET.$.R*.WOBBLE returned %d and FTYPE %d\n", result, p.ftype);

		result = fs_normalize_path_wildcard(old_fs_count, 0, ":ECONET.$.R*.WOBBLE*", -1, &p, 1); // Should give us 1 & FS_FTYPE_NOTFOUND

		fprintf (stderr, "Normalize :ECONET.$.R*.WOBBLE* returned %d and FTYPE %d\n", result, p.ftype);

		// End of Wildcard test harness 
*/

		if (!fs_quiet) fprintf (stderr, "   FS: Server %d successfully initialized\n", old_fs_count);
		return old_fs_count; // The index of the newly initialized server
	}
}

// Used when we must be able to specify a ctrl byte

void fs_error_ctrl(int server, unsigned char reply_port, unsigned char net, unsigned char stn, unsigned char ctrl, unsigned char error, char *msg)
{
	struct __econet_packet_udp reply;

	reply.p.port = reply_port;
	reply.p.ctrl = ctrl;
	reply.p.ptype = ECONET_AUN_DATA;
	reply.p.data[0] = 0x00;
	reply.p.data[1] = error;
	memcpy (&(reply.p.data[2]), msg, strlen((const char *) msg));
	reply.p.data[2+strlen(msg)] = 0x0d;

	// 8 = UDP Econet header, 2 = 0 and then error code, rest is message + 0x0d
	fs_aun_send (&reply, server, 2+(strlen(msg))+1, net, stn);

}

// Used when we don't need to send a particular control byte back
void fs_error(int server, unsigned char reply_port, unsigned char net, unsigned char stn, unsigned char error, char *msg)
{
	fs_error_ctrl(server, reply_port, net, stn, 0x80, error, msg);
}

void fs_reply_ok(int server, unsigned char reply_port, unsigned char net, unsigned char stn)
{

	struct __econet_packet_udp reply;

	reply.p.port = reply_port;
	reply.p.ctrl = 0x80;
	reply.p.seq = (fs_stations[server].seq += 4);
	reply.p.pad = 0x00;
	reply.p.ptype = ECONET_AUN_DATA;
	reply.p.data[0] = 0x00;
	reply.p.data[1] = 0x00;

	fs_aun_send (&reply, server, 2, net, stn);
}

void fs_toupper(char *a)
{
	unsigned short counter = 0;

	while (*(a+counter) != 0)
	{
		if (*(a+counter) >= 'a' && *(a+counter) <= 'z')
			*(a+counter) -= 32;
		counter++;
	}

}

unsigned short fs_find_bulk_port(int server)
{
	int portcount;
	unsigned short found = 0;

	portcount = 1; // Don't try port 0... immediates!

	while (!found && portcount < 255)
	{
		if ((fs_bulk_ports[server][portcount].handle == -1) && (portcount != 0x99) && (portcount != 0xd1) && (portcount != 0x9f) && (portcount != 0xf0)) // 0xd1, 9f are print server; f0 will be the port server, 0x99 is the fileserver...
			found = 1;
		else portcount++;
	}

	if (found) return portcount;
	else return 0;
}

int fs_stn_logged_in(int server, unsigned char net, unsigned char stn)
{

	int count;

	short found = 0;

	count = 0;

	while (!found && (count < ECONET_MAX_FS_USERS))
	{
		if (	(active[server][count].net == net) &&
			(active[server][count].stn == stn) )
			return count;
		count++;
	}

	return -1;	
}

void fs_bye(int server, unsigned char reply_port, unsigned char net, unsigned char stn)
{

	struct __econet_packet_udp reply;
	int active_id;
	int count;

	active_id = fs_stn_logged_in(server, net, stn);

	if (!fs_quiet) fprintf (stderr, "   FS:            from %3d.%3d Bye\n", net, stn);

	// Close active files / handles

	
	count = 1;
	while (count < FS_MAX_OPEN_FILES)
	{
		if (active[server][active_id].fhandles[count].handle != -1 && active[server][active_id].fhandles[count].is_dir)
			fs_deallocate_user_dir_channel(server, active_id, count);
		count++;
	}

	count = 1;
	while (count < FS_MAX_OPEN_FILES)
	{
		if (active[server][active_id].fhandles[count].handle != -1 && (active[server][active_id].fhandles[count].is_dir == 0))
		{
			fs_close_interlock(server, active[server][active_id].fhandles[count].handle, active[server][active_id].fhandles[count].mode);
			fs_deallocate_user_file_channel(server, active_id, count);
		}
		count++;
	}

	//fprintf (stderr, "FS doing memset(%8p, 0, %d)\n", &(active[fs_stn_logged_in(server, net, stn)]), sizeof(active)/ECONET_MAX_FS_SERVERS);
	//fprintf (stderr, "FS bulk ports array at %8p\n", fs_bulk_ports[server]);
	memset(&(active[fs_stn_logged_in(server, net, stn)]), 0, sizeof(active) / ECONET_MAX_FS_SERVERS);
	
	reply.p.port = reply_port;
	reply.p.ctrl = 0x80;
	reply.p.data[0] = reply.p.data[1] = 0;

	fs_aun_send(&reply, server, 2, net, stn);
}

void fs_change_pw(int server, unsigned char reply_port, unsigned int userid, unsigned short net, unsigned short stn, unsigned char *params)
{
	char pw_cur[7], pw_new[7], pw_old[7];
	int ptr;
	int new_ptr;

	if (users[server][userid].priv & FS_PRIV_NOPASSWORDCHANGE)
	{
		fs_error(server, reply_port, net, stn, 0xBA, "Insufficient privilege");
		return;
	}

	strncpy((char * ) pw_cur, (const char * ) users[server][userid].password, 6);
	pw_cur[6] = '\0';

	// Find end of current password in params
	ptr = 0;
	while (ptr < strlen(params) && *(params+ptr) != 0x0d && *(params+ptr) != ' ')
	{
		pw_old[ptr] = *(params+ptr);
		ptr++;
	}

	new_ptr = ptr; // Temp use of new_ptr
	while (new_ptr < 6) pw_old[new_ptr++] = ' ';
	pw_old[6] = '\0';

	if (ptr == strlen(params))
		fs_error(server, reply_port, net, stn, 0xFE, "Bad command");
	else
	{

		new_ptr = 0;
		ptr++;

		// Copy new password
		while (ptr < strlen(params) && (*(params+ptr) != 0x0d) & (new_ptr < 6))
			pw_new[new_ptr++] = *(params+ptr++);

		if (new_ptr == 6 && *(params+ptr) != 0x0d)
			fs_error(server, reply_port, net, stn, 0xFE, "Bad command");
		else
		{	
			for (; new_ptr < 6; new_ptr++)	pw_new[new_ptr] = ' ';

			pw_new[6] = '\0';

			if (	(*params == '\"' && *(params+1) == '\"' && !strcmp(pw_cur, "      "))    // Existing password blank and pass command starts with ""
				||	!strncasecmp((const char *) pw_cur, pw_old, 6))
			{
				unsigned char username[10];
				unsigned char blank_pw[7];
				
				strcpy ((char * ) blank_pw, (const char * ) "      ");

				// Correct current password
				if (!strncmp(pw_new, "\"\"    ", 6)) // user wants to change to blank password
					strncpy((char * ) users[server][userid].password, (const char * ) blank_pw, 6);
				else
					strncpy((char * ) users[server][userid].password, (const char * ) pw_new, 6);
				fs_write_user(server, userid, (char *) &(users[server][userid]));	
				fs_reply_success(server, reply_port, net, stn, 0, 0);
				strncpy((char * ) username, (const char * ) users[server][userid].username, 10);
				username[10] = 0;
				if (!fs_quiet) fprintf (stderr, "   FS: User %s changed password\n", username);
			}
			else	fs_error(server, reply_port, net, stn, 0xB9, "Bad password");
		}
	}

}

// Set boot option
void fs_set_bootopt(int server, unsigned char reply_port, unsigned int userid, unsigned short net, unsigned short stn, unsigned char *data)
{

	unsigned char new_bootopt;

	new_bootopt = *(data+5);

	if (new_bootopt > 7)
	{
		fs_error(server, reply_port, net, stn, 0xBD, "Bad option");
		return;
	}

	if (!fs_quiet) fprintf (stderr, "   FS:%12sfrom %3d.%3d Set boot option %d\n", "", net, stn, new_bootopt);
	
	users[server][userid].bootopt = new_bootopt;
	active[server][fs_stn_logged_in(server,net,stn)].bootopt = new_bootopt;
	fs_write_user(server, userid, (char *) &(users[server][userid]));

	fs_reply_success(server, reply_port, net, stn, 0, 0);
	return;


}

void fs_login(int server, unsigned char reply_port, unsigned char net, unsigned char stn, unsigned char *command)
{

	char username[11];
	char password[7];

	unsigned short counter, stringptr;
	unsigned short found = 0;

	fs_toupper(command);
	memset (username, ' ', 10);
	memset (password, ' ', 6);

	stringptr = counter = 0; // Pointer in command now starts where the start of the username should be

	// Skip station number if provided

	if (isdigit(*command))
	{
		while ((*(command+stringptr) != ' ') && (stringptr < strlen(command))) stringptr++;
		// Now skip any spaces
		while ((*(command+stringptr) == ' ') && (stringptr < strlen(command))) stringptr++;
	}

	if (stringptr == strlen(command)) // Garbled *IAM
	{
		fs_error (server, reply_port, net, stn, 0xFF, "Garbled login command");
		return;
	}

	while (*(command + stringptr) != ' ' && *(command + stringptr) != 0 && (counter < 10))
	{
		username[counter] = *(command + stringptr);
		counter++;
		stringptr++;
	}

	username[10] = 0; // Terminate for logging purposes

	// Skip any whitespace
	while ((*(command + stringptr) == ' ') && (stringptr < strlen(command)))	stringptr++;

	if (*(command + stringptr) != 0) // There's a password too
	{
		unsigned short pw_counter = 0;

		counter++;
	
		while ((*(command + stringptr) != 0x00) && (pw_counter < 6))
		{
			password[pw_counter++] = *(command + stringptr);
			stringptr++;
		}

		for (; pw_counter < 6; pw_counter++) password[pw_counter] = ' ';
	}


	password[6] = 0; // Terminate for logging purposes

	counter = 0;

	while (counter < fs_stations[server].total_users && !found)
	{
		if (!strncmp(users[server][counter].username, username, 10) && (users[server][counter].priv != 0))
			found = 1;
		else
			counter++;
	}

	if (found)
	{
		if (strncasecmp((const char *) users[server][counter].password, password, 6))
		{
			fs_error(server, reply_port, net, stn, 0xBC, "Wrong password");
			if (!fs_quiet) fprintf (stderr, "   FS:            from %3d.%3d Login attempt - username '%s' - Wrong password\n", net, stn, username);
		}
		else if (users[server][counter].priv & FS_PRIV_LOCKED)
		{
			fs_error(server, reply_port, net, stn, 0xBC, "Account locked");
			if (!fs_quiet) fprintf (stderr, "   FS:            from %3d.%3d Login attempt - username '%s' - Account locked\n", net, stn, username);
		}
		else
		{
			int usercount = 0;
			short found = 0;	
			
			// Find a spare slot

			while (!found && (usercount < ECONET_MAX_FS_USERS))
			{
				if ((active[server][usercount].net == 0 && active[server][usercount].stn == 0) ||
				    (active[server][usercount].net == net && active[server][usercount].stn == stn)) // Allows us to overwrite an existing handle if the station is already logged in
					found = 1;
				else usercount++;
			}

			if (!found)
			{
				if (!fs_quiet) fprintf (stderr, "   FS:            from %3d.%3d Login attempt - username '%s' - server full\n", net, stn, username);
				fs_error(server, reply_port, net, stn, 0xB8, "Too many users");
			}
			else
			{
				short internal_handle; 
				char home[96], lib[96];
				struct path p;
				unsigned short count;

				struct __econet_packet_udp reply;

				if (fs_stn_logged_in(server, net, stn) != -1) // do a bye first
					fs_bye(server, reply_port, net, stn);

				active[server][usercount].net = net;
				active[server][usercount].stn = stn;
				active[server][usercount].userid = counter;
				active[server][usercount].bootopt = users[server][counter].bootopt;
				active[server][usercount].priv = users[server][counter].priv;
				active[server][usercount].userid = counter;
				active[server][usercount].current_disc = users[server][counter].home_disc;

				for (count = 0; count < FS_MAX_OPEN_FILES; count++) active[server][usercount].dhandles[count].handle = -1; // Flag unused for directories
				for (count = 0; count < FS_MAX_OPEN_FILES; count++) active[server][usercount].fhandles[count].handle = -1; // Flag unused for files

				strncpy((char * ) home, (const char * ) users[server][counter].home, 96);
				home[96] = '\0';
				for (count = 0; count < 96; count++) if (home[count] == 0x20) home[count] = '\0'; // Remove spaces and null terminate

				// First, root

				if (!fs_normalize_path(server, usercount, "$", -1, &p)) // NOTE: because fs_normalize might look up current or home directory, home must be a complete path from $
				{

					if (!fs_quiet) fprintf (stderr, "   FS:%12sfrom %3d.%3d Login attempt - cannot find root dir %s\n", "", net, stn, home);
					fs_error (server, reply_port, net, stn, 0xFF, "Unable to map root.");
					active[server][usercount].net = 0; active[server][usercount].stn = 0; return;
				}
						
				if (p.ftype != FS_FTYPE_DIR) // Root wasn't a directory!
				{
					fs_error (server, reply_port, net, stn, 0xA8, "Bad root directory.");
					active[server][usercount].net = 0; active[server][usercount].stn = 0; return;
				}
					
				if ((internal_handle = fs_get_dir_handle(server, usercount, p.unixpath)) == -1)
				{
					fs_error (server, reply_port, net, stn, 0xA8, "Root directory inaccessible!");
					active[server][usercount].net = 0; active[server][usercount].stn = 0; return;
				}

				if ((active[server][usercount].root = fs_allocate_user_dir_channel(server, usercount, internal_handle)) == -1) // Can't allocate
				{
					fs_error (server, reply_port, net, stn, 0xDE, "Root directory channel ?");
					fs_close_dir_handle(server, internal_handle);
					active[server][usercount].net = 0; active[server][usercount].stn = 0; return;
				}

				strcpy(active[server][usercount].fhandles[active[server][usercount].root].acornfullpath, p.acornfullpath);

				strncpy((char * ) active[server][usercount].root_dir, (const char * ) "", 11);
				strncpy((char * ) active[server][usercount].root_dir_tail, (const char * ) "$         ", 11);
	
				// Next, CWD, which starts as home	
				
				if (!fs_normalize_path(server, usercount, home, -1, &p)) // NOTE: because fs_normalize might look up current or home directory, home must be a complete path from $
				{

					if (!fs_quiet) fprintf (stderr, "   FS:%12sfrom %3d.%3d Login attempt - cannot find home dir %s\n", "", net, stn, home);
					if (!fs_normalize_path(server, usercount, "$", -1, &p)) // Use root as home directory instead
					{
						fs_error (server, reply_port, net, stn, 0xA8, "Unable to map home.");
						active[server][usercount].net = 0; active[server][usercount].stn = 0; return;
					}

				}
						
				if (p.ftype != FS_FTYPE_DIR) // Homedir wasn't a directory!
				{
					fs_error (server, reply_port, net, stn, 0xA8, "Bad home directory.");
					active[server][usercount].net = 0; active[server][usercount].stn = 0; return;
				}
					
				if ((internal_handle = fs_get_dir_handle(server, usercount, p.unixpath)) == -1)
				{
					fs_error (server, reply_port, net, stn, 0xA8, "Home directory not found");
					active[server][usercount].net = 0; active[server][usercount].stn = 0; return;
				}

				if ((active[server][usercount].current = fs_allocate_user_dir_channel(server, usercount, internal_handle)) == -1) // Can't allocate
				{
					fs_error (server, reply_port, net, stn, 0xDE, "Current dir channel ?");
					active[server][usercount].net = 0; active[server][usercount].stn = 0; return;
					// Don't close the dir - it hasn't been clocked as open internally by another reader because fs_alloc... failed.
				}

				strcpy(active[server][usercount].fhandles[active[server][usercount].current].acornfullpath, p.acornfullpath);

				strncpy((char * ) active[server][usercount].current_dir, (const char * ) p.path_from_root, 255); // Current starts at home
				if (p.npath == 0)	sprintf(active[server][usercount].current_dir_tail, "$         ");
				else			sprintf(active[server][usercount].current_dir_tail, "%-10s", p.path[p.npath-1]);


				// Next, Library

				strncpy((char * ) lib, (const char * ) users[server][counter].lib, 96);
				lib[96] = '\0';
				for (count = 0; count < 96; count++) if (lib[count] == 0x20) lib[count] = '\0'; // Remove spaces and null terminate

				if (!fs_normalize_path(server, usercount, lib, -1, &p) || p.ftype != FS_FTYPE_DIR) // NOTE: because fs_normalize might look up current or home directory, home must be a complete path from $
				{

					if (!fs_quiet) fprintf (stderr, "   FS:%12sfrom %3d.%3d Login attempt - cannot find lib dir %s\n", "", net, stn, lib);
					if (!fs_normalize_path(server, usercount, "$", -1, &p)) // Use root as library directory instead
					{
						fs_error (server, reply_port, net, stn, 0xA8, "Unable to map library");
						active[server][usercount].net = 0; active[server][usercount].stn = 0; return;
					}

				}
						
				if (p.ftype != FS_FTYPE_DIR) // Libdir wasn't a directory!
				{
					fs_error (server, reply_port, net, stn, 0xA8, "Bad library directory.");
					active[server][usercount].net = 0; active[server][usercount].stn = 0; return;
				}
					
				if ((internal_handle = fs_get_dir_handle(server, usercount, p.unixpath)) == -1)
				{
					fs_error (server, reply_port, net, stn, 0xA8, "Library directory not found");
					active[server][usercount].net = 0; active[server][usercount].stn = 0; return;
				}

				if ((active[server][usercount].lib = fs_allocate_user_dir_channel(server, usercount, internal_handle)) == -1) // Can't allocate
				{
					fs_error (server, reply_port, net, stn, 0xDE, "Library dir channel ?");
					fs_close_dir_handle(server, internal_handle);
					active[server][usercount].net = 0; active[server][usercount].stn = 0; return;
				}
	
				strcpy(active[server][usercount].fhandles[active[server][usercount].lib].acornfullpath, p.acornfullpath);

				strncpy((char * ) active[server][usercount].lib_dir, (const char * ) p.path_from_root, 255);
				if (p.npath == 0)
					strcpy((char * ) active[server][usercount].lib_dir_tail, (const char * ) "$         ");
				else
					sprintf(active[server][usercount].lib_dir_tail, "%-10s", p.path[p.npath-1]);

				if (!fs_quiet) fprintf (stderr, "   FS:            from %3d.%3d Login as %s, index %d, id %d, disc %d, root %s, priv 0x%02x\n", net, stn, username, usercount, active[server][usercount].userid, active[server][usercount].current_disc, active[server][usercount].root_dir, active[server][usercount].priv);

				// Tell the station
			
				reply.p.ptype = ECONET_AUN_DATA;
				reply.p.port = reply_port;
				reply.p.ctrl = 0x80;
				reply.p.pad = 0x00;
				reply.p.seq = (fs_stations[server].seq += 4);
				reply.p.data[0] = 0x05;
				reply.p.data[1] = 0x00;
				reply.p.data[2] = active[server][usercount].root;
				reply.p.data[3] = active[server][usercount].current;
				reply.p.data[4] = active[server][usercount].lib;
				reply.p.data[5] = active[server][usercount].bootopt;
				
				fs_aun_send(&reply, server, 6, net, stn);
			}
		}

	}
	else
	{
		if (!fs_quiet) fprintf (stderr, "   FS:            from %3d.%3d Login attempt - username '%s' - Unknown user\n", net, stn, username);
		fs_error(server, reply_port, net, stn, 0xBC, "User not known");
	}

}

void fs_read_user_env(int server, unsigned short reply_port, unsigned char net, unsigned char stn, unsigned int active_id)
{

	struct __econet_packet_udp r;
	int replylen = 0;
	unsigned short disclen;

	if (!fs_quiet) fprintf (stderr, "   FS:%12sfrom %3d.%3d Read user environment\n", "", net, stn);

	r.p.port = reply_port;
	r.p.ctrl = 0x80;
	r.p.ptype = ECONET_AUN_DATA;

	r.p.data[replylen++] = 0;
	r.p.data[replylen++] = 0;

	disclen = r.p.data[replylen++] = 16; // strlen(fs_discs[server][active[server][active_id].disc].name);

	sprintf (&(r.p.data[replylen]), "%-16s", fs_discs[server][active[server][active_id].current_disc].name);

	replylen += disclen;

	sprintf (&(r.p.data[replylen]), "%-10s", active[server][active_id].current_dir_tail);
	replylen += 10;

	sprintf (&(r.p.data[replylen]), "%-10s", active[server][active_id].lib_dir_tail);
	replylen += 10;

	fs_aun_send (&r, server, replylen, net, stn);
	
}

void fs_examine(int server, unsigned short reply_port, unsigned char net, unsigned char stn, unsigned int active_id, unsigned char *data, unsigned int datalen)
{
	unsigned short relative_to, arg, start, n;
	unsigned char path[256];
	struct path p;
	struct path_entry *e;
	struct __econet_packet_udp r;
	int replylen;
	unsigned short examined, dirsize;
	// Next 4 lines only used in the old non-wildcard code
	//DIR *d;
	//struct dirent *entry;
	//struct objattr attr;
	//char unixpath[1024];
	char acornpathfromroot[1024];

	relative_to = *(data+3);
	arg = (char) *(data+5);
	start = (char) *(data+6);
	n = (char) *(data+7);

	fs_copy_to_cr(path, (data + 8), 255);

	if (arg == 2) // If arg = 2, it looks like the path to examine starts at data + 9 and ends with the end of the packet, not 0x0d
	{
		unsigned short p;

		strncpy(path, (data + 9), datalen - 9);
		path[datalen - 9] = '\0';	
		// But sometimes it sticks 0x0d on the end
		// So ferret it out and remove

		p = 0;
	
		while (p <= datalen - 9)
		{
			if (path[p] == 0x0d) path[p] = '\0';
			p++;
		}
		
	}

	if (!fs_quiet) fprintf(stderr, "   FS:%12sfrom %3d.%3d Examine %s relative to %d, start %d, extent %d, arg = %d\n", "", net, stn, path,
		relative_to, start, n, arg);

	if (!fs_normalize_path_wildcard(server, active_id, path, relative_to, &p, 1)) // || p.ftype == FS_FTYPE_NOTFOUND)
	{

		fs_error(server, reply_port, net, stn, 0xD6, "Not found");
		return;

		struct __econet_packet_udp reply;
	
		reply.p.ptype = ECONET_AUN_DATA;
		reply.p.port = reply_port;
		reply.p.ctrl = 0x80;
		reply.p.data[0] = reply.p.data[1] = reply.p.data[2] = 0; // This is apparently how you flag not found on an examine...
	
		fs_aun_send(&reply, server, 2, net, stn);
		return;
	}

	// Add final entry onto path_from_root (because normalize doesn't do it on a wildcard call)

	if (strlen(p.path_from_root) != 0)
		strcat(p.path_from_root, ".");
	if (p.paths != NULL)
		strcat (p.path_from_root, p.paths->acornname);

	fs_free_wildcard_list(&p); // We'll just use the first one it found, which will be in the main path struct

	if (p.ftype != FS_FTYPE_DIR)
	{
		fs_error(server, reply_port, net, stn, 0xAF, "Types don't match");
		return;
	}

	replylen = 0;

	r.p.ptype = ECONET_AUN_DATA;
	r.p.port = reply_port;
	r.p.ctrl = 0x80;

	r.p.data[replylen++] = 0;
	r.p.data[replylen++] = 0;
	
	examined = r.p.data[replylen++] = 0; // Repopulate data[2] at end
	dirsize = r.p.data[replylen++] = 0; // Dir size (but this might be wrong). Repopulate later if correct

	// Wildcard code
	strcpy(acornpathfromroot, path);
	if (strlen(acornpathfromroot) != 0) strcat(acornpathfromroot, ".");
	strcat(acornpathfromroot, "*"); // It should already have $ on it if root.

	// Wildcard renormalize
	if (!fs_normalize_path_wildcard(server, active_id, acornpathfromroot, relative_to, &p, 1)) // || p.ftype == FS_FTYPE_NOTFOUND)
	{
		fs_error(server, reply_port, net, stn, 0xD6, "Not found");
		return;
	}

	e = p.paths;
	while (dirsize < start && (e != NULL))
	{
		if ((e->perm & FS_PERM_H) == 0 || (e->owner == active[server][active_id].userid)) // not hidden
			dirsize++;
		e = e->next;
	}

	while (examined < n && (e != NULL))
	{	
		if ((e->perm & FS_PERM_H) == 0 || (e->owner == active[server][active_id].userid)) // not hidden or we are the owner
		{
			switch (arg)
			{
				case 0: // Machine readable format
				{
					r.p.data[replylen] = htole32(e->load); replylen += 4;
					r.p.data[replylen] = htole32(e->exec); replylen += 4;
					r.p.data[replylen++] = e->perm;
					r.p.data[replylen++] = e->day;
					r.p.data[replylen++] = e->monthyear;
					r.p.data[replylen++] = e->internal & 0xff;
					r.p.data[replylen++] = (e->internal & 0xff00) >> 8;
					r.p.data[replylen++] = (e->internal & 0xff00) >> 16;
					r.p.data[replylen++] = e->length & 0xff;
					r.p.data[replylen++] = (e->length & 0xff00) >> 8;
					r.p.data[replylen++] = (e->length & 0xff00) >> 16;
				} break;
				case 1: // Human readable format
				{
					unsigned char tmp[256];
					unsigned char permstring_l[10], permstring_r[10];
	
					sprintf(permstring_l, "%s%s%s%s",
						(e->ftype == FS_FTYPE_DIR ? "D" : e->ftype == FS_FTYPE_SPECIAL ? "S" : ""),
						((e->perm & FS_PERM_L) ? "L" : ""),
						((e->perm & FS_PERM_OWN_W) ? "W" : ""),
						((e->perm & FS_PERM_OWN_R) ? "R" : "") );

					sprintf(permstring_r, "%s%s", 
						((e->perm & FS_PERM_OTH_W) ? "W" : ""),
						((e->perm & FS_PERM_OTH_R) ? "R" : "") );

					sprintf (tmp, "%-10s %08lX %08lX   %06lX   %4s/%-2s     %02d/%02d/%02d %06lX", e->acornname,
						e->load, e->exec, e->length,
						permstring_l, permstring_r,
						fs_day_from_two_bytes(e->day, e->monthyear),
						fs_month_from_two_bytes(e->day, e->monthyear),
						fs_year_from_two_bytes(e->day, e->monthyear),
						e->internal
						);
						
					strcpy((char * ) &(r.p.data[replylen]), (const char * ) tmp);
					replylen += strlen(tmp);
					r.p.data[replylen++] = '\0';

				} break;
				case 2: // 10 character filename format (short)
				{
					r.p.data[replylen++] = 0x0a;
					sprintf((char *) &(r.p.data[replylen]), "%-10s", e->acornname);
					replylen += 10;

				} break;
				case 3: // 10 character filename format (long)
				{
					char tmp[256];
					char permstring_l[10], permstring_r[10];

					sprintf(permstring_l, "%s%s%s%s",
						(e->ftype == FS_FTYPE_DIR ? "D" : e->ftype == FS_FTYPE_SPECIAL ? "S" : ""),
						((e->perm & FS_PERM_L) ? "L" : ""),
						((e->perm & FS_PERM_OWN_W) ? "W" : ""),
						((e->perm & FS_PERM_OWN_R) ? "R" : "") );

					sprintf(permstring_r, "%s%s", 
						((e->perm & FS_PERM_OTH_W) ? "W" : ""),
						((e->perm & FS_PERM_OTH_R) ? "R" : "") );

					sprintf (tmp, "%-10s %4s/%-2s", e->acornname,
						permstring_l, permstring_r
					);
					strcpy((char * ) &(r.p.data[replylen]), (const char * ) tmp);
					replylen += strlen(tmp) + 1; // +1 for the 0 byte
				} break;
			}
			examined++;
			dirsize++;
		}

		e = e->next;

	}

	fs_free_wildcard_list(&p);

// OLD non-wildcard code
/*

	if (!(d = opendir(p.unixpath)))
	{
		fs_error(server, reply_port, net, stn, 0xA8, "Broken dir");
		return;
	}

	// Skip to start entry

	while ((dirsize < start) && (entry = readdir(d)))
	{
		// Ignore special files
		if ((strlen(entry->d_name) <= 10) && (strcmp(entry->d_name, ".")) && (strcmp(entry->d_name, "..")) && strcasecmp(entry->d_name, "lost+found"))
		{
			strcpy((char * ) unixpath, (const char * ) p.unixpath);
			strcat(unixpath, "/");
			strcat(unixpath, entry->d_name);	

			fs_read_xattr(unixpath, &attr);

			if ((attr.perm & FS_PERM_H) == 0 || (attr.owner == active[server][active_id].userid)) // not hidden
				dirsize++;
			//fprintf (stderr, "Skipped %s\n", entry->d_name);
		}
	}

	//fprintf (stderr, "After skipping, examined = %d, n = %d\n", examined, n);

	while ((examined < n) && (entry = readdir(d)))
	{
		struct path file;
		char acorn_name[11];
		char fullpath[1024];

		if ((strlen(entry->d_name) <= 10) && (strcmp(entry->d_name, ".")) && (strcmp(entry->d_name, "..")) && strcasecmp(entry->d_name, "lost+found"))
		{
			strncpy ((char * ) acorn_name, (const char * ) entry->d_name, 10);
			fs_unix_to_acorn(acorn_name); // Starts in unix format; this puts it into acorn format, as the variable name suggests.
	
			sprintf(fullpath, ":%s.%s%s%s", p.discname, p.path_from_root, (p.npath > 0) ? "." : "", acorn_name);

			//fprintf(stderr, "Calling normalize() on %s\n", fullpath);

			if (!fs_normalize_path(server, active_id, fullpath, -1, &file))
			{
				fs_error(server, reply_port, net, stn, 0xA8, "Broken dir");
				closedir(d);
				return;
			}

			if ((file.perm & FS_PERM_H) == 0 || (file.owner == active[server][active_id].userid)) // not hidden or we are the owner
			{
				switch (arg)
				{
					case 0: // Machine readable format
					{
						r.p.data[replylen] = htole32(file.load); replylen += 4;
						r.p.data[replylen] = htole32(file.exec); replylen += 4;
						r.p.data[replylen++] = file.perm;
						r.p.data[replylen++] = file.day;
						r.p.data[replylen++] = file.monthyear;
						r.p.data[replylen++] = file.internal & 0xff;
						r.p.data[replylen++] = (file.internal & 0xff00) >> 8;
						r.p.data[replylen++] = (file.internal & 0xff00) >> 16;
						r.p.data[replylen++] = file.length & 0xff;
						r.p.data[replylen++] = (file.length & 0xff00) >> 8;
						r.p.data[replylen++] = (file.length & 0xff00) >> 16;
					} break;
					case 1: // Human readable format
					{
						unsigned char tmp[256];
						unsigned char permstring_l[10], permstring_r[10];
		
						sprintf(permstring_l, "%s%s%s%s",
							(file.ftype == FS_FTYPE_DIR ? "D" : file.ftype == FS_FTYPE_SPECIAL ? "S" : ""),
							((file.perm & FS_PERM_L) ? "L" : ""),
							((file.perm & FS_PERM_OWN_W) ? "W" : ""),
							((file.perm & FS_PERM_OWN_R) ? "R" : "") );

						sprintf(permstring_r, "%s%s", 
							((file.perm & FS_PERM_OTH_W) ? "W" : ""),
							((file.perm & FS_PERM_OTH_R) ? "R" : "") );

						sprintf (tmp, "%-10s %08lX %08lX   %06lX   %4s/%-2s     %02d/%02d/%02d %06lX", (file.npath == 0) ? (char *) "$" : (char *) file.path[file.npath - 1],
							file.load, file.exec, file.length,
							permstring_l, permstring_r,
							file.day, file.monthyear & 0x0f, ((file.monthyear & 0xf0) >> 4) + 81,
							file.internal
							);
							
						strcpy((char * ) &(r.p.data[replylen]), (const char * ) tmp);
						replylen += strlen(tmp);
						r.p.data[replylen++] = '\0';

					} break;
					case 2: // 10 character filename format (short)
					{
						r.p.data[replylen++] = 0x0a;
						sprintf((char *) &(r.p.data[replylen]), "%-10s", (file.npath == 0) ? (char *) "$" : (char *) file.path[file.npath - 1]);
						replylen += 10;

					} break;
					case 3: // 10 character filename format (long)
					{
						char tmp[256];
						char permstring_l[10], permstring_r[10];

						sprintf(permstring_l, "%s%s%s%s",
							(file.ftype == FS_FTYPE_DIR ? "D" : file.ftype == FS_FTYPE_SPECIAL ? "S" : ""),
							((file.perm & FS_PERM_L) ? "L" : ""),
							((file.perm & FS_PERM_OWN_W) ? "W" : ""),
							((file.perm & FS_PERM_OWN_R) ? "R" : "") );

						sprintf(permstring_r, "%s%s", 
							((file.perm & FS_PERM_OTH_W) ? "W" : ""),
							((file.perm & FS_PERM_OTH_R) ? "R" : "") );

						sprintf (tmp, "%-10s %4s/%-2s", (file.npath == 0) ? (char *) "$" : (char *) file.path[file.npath - 1],
							permstring_l, permstring_r
						);
						strcpy((char * ) &(r.p.data[replylen]), (const char * ) tmp);
						replylen += strlen(tmp) + 1; // +1 for the 0 byte
					} break;
				}
				examined++;
				dirsize++;
			}

		}

	}

	while ((entry = readdir(d))) // Count any remaining entries
	{

		if ((strcmp(entry->d_name, ".")) && (strcmp(entry->d_name, "..")))
		{
			strcpy((char * ) unixpath, (const char * ) p.unixpath);
			strcat(unixpath, "/");
			strcat(unixpath, entry->d_name);	
			fs_read_xattr(unixpath, &attr);

			if ((attr.perm & FS_PERM_H) == 0) // not hidden
				dirsize++;
		}
	}
*/	
	r.p.data[replylen++] = 0x80;
	r.p.data[2] = (examined & 0xff);
	r.p.data[3] = (examined & 0xff); // Can't work out how L3 is calculating this number

/* OLD non-wildcard code
	closedir (d);
*/

	fs_aun_send(&r, server, replylen, net, stn);

}

void fs_set_object_info(int server, unsigned short reply_port, unsigned char net, unsigned char stn, unsigned int active_id, unsigned char *data, unsigned int datalen)
{

	unsigned short relative_to;

	struct __econet_packet_udp r;

	unsigned short command;

	char path[1024];

	unsigned short filenameposition;
		
	struct path p;

	command = *(data+5);
	relative_to = *(data+3);

	// So what's in 4?

	switch (command)
	{
		case 1: filenameposition = 15; break;
		case 4: filenameposition = 7; break;
		case 2: // Fall through
		case 3: // Fall through
		case 5: filenameposition = 10; break;
		default:
			fs_error(server, reply_port, net, stn, 0xFF, "FS Error");
			break;
	}

	fs_copy_to_cr(path, (data+filenameposition), 1023);

	if (!fs_quiet) fprintf (stderr, "   FS:%12sfrom %3d.%3d Set Object Info %s relative to %s, command %d\n", "", net, stn, path, relative_to == active[server][active_id].root ? "Root" : relative_to == active[server][active_id].lib ? "Library" : "Current", command);
	
	if (!fs_normalize_path(server, active_id, path, relative_to, &p) || p.ftype == FS_FTYPE_NOTFOUND)
		fs_error(server, reply_port, net, stn, 0xD6, "Not found");
	else if (((active[server][active_id].priv & FS_PRIV_SYSTEM) == 0) && 
			(p.owner != active[server][active_id].userid) &&
			(p.parent_owner != active[server][active_id].userid)
		)
		fs_error(server, reply_port, net, stn, 0xBD, "Insufficient access");
	else if (command != 1 && command != 4 && (p.perm & FS_PERM_L)) // Locked
		fs_error(server, reply_port, net, stn, 0xC3, "Locked");
	else
	{
		struct objattr attr;
	
		r.p.ptype = ECONET_AUN_DATA;
		r.p.port = reply_port;
		r.p.ctrl = 0x80;
		r.p.data[0] = r.p.data[1] = 0;

		fs_read_xattr(p.unixpath, &attr);

		switch (command)
		{
			case 1: // Set Load, Exec & Attributes
			
				attr.load = (*(data+6)) + (*(data+7) << 8) + (*(data+8) << 16) + (*(data+9) << 24);
				attr.exec = (*(data+10)) + (*(data+11) << 8) + (*(data+12) << 16) + (*(data+13) << 24);
				// We need to make sure our bitwise stuff corresponds with Acorns before we do this...
				// attr.perm = (*(data+14));
				break;
			
			case 2: // Set load address
				attr.load = (*(data+6)) + (*(data+7) << 8) + (*(data+8) << 16) + (*(data+9) << 24);
				break;

			case 3: // Set exec address
				attr.exec = (*(data+6)) + (*(data+7) << 8) + (*(data+8) << 16) + (*(data+9) << 24);
				break;
	
			case 4: // Set attributes only
				// attr.perm = (*(data+6));
				break;

			case 5: // Set file date
			{
				// Not sure what is in *(data+6 .. 9) yet! Do nothing for now.	
			}
				break;

			// No default needed - we caught it above
		}

		fs_write_xattr(p.unixpath, attr.owner, attr.perm, attr.load, attr.exec);

		// If we get here, we need to send the reply

		fs_aun_send(&r, server, 2, net, stn);

	}
}

int fs_scandir_regex(const struct dirent *d)
{

	return (((strcasecmp(d->d_name, "lost+found") == 0) || (regexec(&r_wildcard, d->d_name, 0, NULL, 0) != 0)) ? 0 : 1); // regexec returns 0 on match, so we need to return 0 (no match) if it returns other than 0.

}

// Frees up malloc'd dirent entries from scandir
void fs_free_dirent(struct dirent **list, int entries)
{

	while (entries--)
		free(list[entries]);

	free(list);

}

// Counts number of Acorn-compatible entries in unixpath.
// Returns the number found, or -1 for failure

short fs_get_acorn_entries(int server, int active_id, char *unixpath)
{

	int entries;
	char regex[1024];
	struct dirent **list;

	sprintf(regex, "^(%s{1,10})", FSREGEX);

	if (regcomp(&r_wildcard, regex, REG_EXTENDED | REG_ICASE | REG_NOSUB) != 0) // We go extended expression, case insensitive and we aren't bothered about finding *where* the matches are in the string
		return -1; // Regex failure!

	entries = scandir(unixpath, &list, fs_scandir_regex, fs_alphacasesort);

	if (entries == -1) // Failure
		return -1;

	fs_free_dirent(list, entries); // De-malloc everything

	regfree (&r_wildcard);

	return entries;

}

void fs_get_object_info(int server, unsigned short reply_port, unsigned char net, unsigned char stn, unsigned int active_id, unsigned char *data, unsigned int datalen)
{

	unsigned short replylen = 0, relative_to;

	struct __econet_packet_udp r;

	unsigned short command;
	

	unsigned short norm_return;
	char path[1024];
		
	struct path p;

	command = *(data+5);
	relative_to = *(data+3);

	memset(r.p.data, 0, 30);
	r.p.port = reply_port;
	r.p.ctrl = 0;
	r.p.ptype = ECONET_AUN_DATA;

	// Use replylen as a temporary counter

	while (replylen < 1024 && *(data+(command != 3 ? 6 : 10)+replylen) != 0x0d)
	{
		path[replylen] = *(data+(command != 3 ? 6 : 10)+replylen);
		replylen++;
	}

	path[replylen] = '\0'; // Null terminate instead of 0x0d in the packet

	if (!fs_quiet) fprintf (stderr, "   FS:%12sfrom %3d.%3d Get Object Info %s relative to %02X, command %d\n", "", net, stn, path, relative_to, command);
	

	norm_return = fs_normalize_path_wildcard(server, active_id, path, relative_to, &p, 1);

	fs_free_wildcard_list(&p); // Not interested in anything but first entry, which will be in main struct

	if (!norm_return && (p.error != FS_PATH_ERR_NODIR))
	{
		fs_error(server, reply_port, net, stn, 0xcc, "Bad filename");
		return;
	}

	if ((!norm_return && p.error == FS_PATH_ERR_NODIR) || (norm_return && p.ftype == FS_FTYPE_NOTFOUND))
	{
		struct __econet_packet_udp reply;
	
		reply.p.ptype = ECONET_AUN_DATA;
		reply.p.port = reply_port;
		reply.p.ctrl = 0x80;
		reply.p.data[0] = reply.p.data[1] = reply.p.data[2] = 0; // This is apparently how you flag not found on an examine...
	
		fs_aun_send(&reply, server, 3, net, stn);
		return;
	}

	replylen = 0; // Reset after temporary use above

	r.p.data[replylen++] = 0;
	r.p.data[replylen++] = 0;
	r.p.data[replylen++] = p.ftype;

	if (command == 2 || command == 5)
	{
		r.p.data[replylen++] = (p.load & 0xff);
		r.p.data[replylen++] = (p.load & 0xff00) >> 8;
		r.p.data[replylen++] = (p.load & 0xff0000) >> 16;
		r.p.data[replylen++] = (p.load & 0xff000000) >> 24;
		r.p.data[replylen++] = (p.exec & 0xff);
		r.p.data[replylen++] = (p.exec & 0xff00) >> 8;
		r.p.data[replylen++] = (p.exec & 0xff0000) >> 16;
		r.p.data[replylen++] = (p.exec & 0xff000000) >> 24;
	}

	if (command == 3 || command == 5)
	{
		r.p.data[replylen++] = (p.length & 0xff);
		r.p.data[replylen++] = (p.length & 0xff00) >> 8;
		r.p.data[replylen++] = (p.length & 0xff0000) >> 16;
	}

	if (command == 4 || command == 5)
	{
		r.p.data[replylen++] = fs_perm_to_acorn(p.perm, p.ftype);
		//if (p.my_perm & FS_PERM_OWN_R) r.p.data[replylen++] = 0xff; else r.p.data[replylen++] = 0x00;
		r.p.data[replylen++] = (active[server][active_id].userid == p.owner) ? 0x00 : 0xff; 
	}

	if (command == 1 || command == 5)
	{
		//r.p.data[replylen++] = p.monthyear & 0x0f;
		//r.p.data[replylen++] = p.monthyear >> 8;
		r.p.data[replylen++] = p.day;
		r.p.data[replylen++] = p.monthyear;
	}

	if (command == 6)
	{
		if (p.ftype != FS_FTYPE_DIR)
		{
			fs_error(server, reply_port, net, stn, 0xAF, "Types don't match");
			return;
		}

		r.p.data[replylen++] = 0; // Undefined on this command
		r.p.data[replylen++] = 10; // Dir name length

	
		if (p.npath == 0) // Root
			strncpy((char * ) &(r.p.data[replylen]), (const char * ) "$         ", 11);
		else
			snprintf(&(r.p.data[replylen]), 11, "%-10s", (const char * ) p.acornname);

		replylen += 10;

		r.p.data[replylen++] = (active[server][active_id].userid == p.owner) ? 0x00 : 0xff; 

		r.p.data[replylen++] = fs_get_acorn_entries(server, active_id, p.unixpath); // Number of directory entries

	}

	fs_aun_send(&r, server, replylen, net, stn);
		
}

// Save file
void fs_save(int server, unsigned short reply_port, unsigned char net, unsigned char stn, unsigned int active_id, unsigned char *data, int datalen, unsigned char rx_ctrl)
{

	unsigned char incoming_port, ack_port;
	unsigned long load, exec, length;
	char filename[12];

	struct __econet_packet_udp r;

	ack_port = *(data+2);	
	
	// Anyone know what the bytes at data+3, 4 are?

	fs_copy_to_cr(filename, data+16, 10);

	load = 	(*(data+5)) + ((*(data+6)) << 8) + ((*(data+7)) << 16) + ((*(data+8)) << 24);

	exec = 	(*(data+9)) + ((*(data+10)) << 8) + ((*(data+11)) << 16) + ((*(data+12)) << 24);
	
	length = (*(data+13)) + ((*(data+14)) << 8) + ((*(data+15)) << 16);

	if (!fs_quiet) fprintf (stderr, "   FS:%12sfrom %3d.%3d SAVE %s %08lx %08lx %06lx\n", "", net, stn, filename, load, exec, length);

	if ((incoming_port = fs_find_bulk_port(server)))
	{
		struct path p;

		if (fs_normalize_path(server, active_id, filename, active[server][active_id].current, &p))
		{
			// Path found
	
			if (p.perm & FS_PERM_L) // Locked - cannot write
				fs_error(server, reply_port, net, stn, 0xC3, "Locked");
			else if (p.ftype != FS_FTYPE_FILE && p.ftype != FS_FTYPE_NOTFOUND) // Not a file!
				fs_error(server, reply_port, net, stn, 0xBD, "Insufficient access");
			else
			{
				if ((p.my_perm & FS_PERM_OWN_W) || (p.ftype == FS_FTYPE_NOTFOUND && (p.parent_perm & FS_PERM_OWN_W)))
				{
					short internal_handle;

					// Can write to it one way or another
		
					// Use interlock function here
					internal_handle = fs_open_interlock(server, p.unixpath, 3, active[server][active_id].userid);

					if (internal_handle == -3)
						fs_error(server, reply_port, net, stn, 0xC0, "Too many open files");
					else if (internal_handle == -2)
						fs_error(server, reply_port, net, stn, 0xc2, "Already open"); // Interlock failure
					else if (internal_handle == -1)
						fs_error(server, reply_port, net, stn, 0xFF, "FS Error"); // File didn't open when it should
					else
					{
						fs_write_xattr(p.unixpath, active[server][active_id].userid, FS_PERM_OWN_R | FS_PERM_OWN_W, load, exec); 

						r.p.port = reply_port;
						r.p.ctrl = rx_ctrl;
						r.p.ptype = ECONET_AUN_DATA;
			
						r.p.data[0] = r.p.data[1] = 0;
						r.p.data[2] = incoming_port;
						r.p.data[3] = (1280 & 0xff); // maximum tx size
						r.p.data[4] = (1280 & 0xff00) >> 8;
				
						fs_aun_send (&r, server, 5, net, stn);

						if (length == 0)
						{

							// Send a closing ACK

							struct tm t; 
							struct stat s;
							unsigned char day, monthyear;

							day = monthyear = 0;

							if (!stat((const char * ) p.unixpath, &s))
							{
								localtime_r(&(s.st_mtime), &t);
								fs_date_to_two_bytes(t.tm_mday, t.tm_mon+1, t.tm_year, &(monthyear), &(day));
								//day = t.tm_mday;
								//monthyear = (((t.tm_year - 81 - 40) & 0x0f) << 4) | ((t.tm_mon+1) & 0x0f);	
							}	
								
							fs_close_interlock(server, internal_handle, 3);
							r.p.port = reply_port;
							r.p.ctrl = rx_ctrl;
							r.p.ptype = ECONET_AUN_DATA;
							r.p.data[0] = r.p.data[1] = 0;
							r.p.data[2] = FS_PERM_OWN_R | FS_PERM_OWN_W;
							r.p.data[3] = day;
							r.p.data[4] = monthyear;

							fs_aun_send (&r, server, 5, net, stn);
						}
						else
						{
							fs_bulk_ports[server][incoming_port].handle = internal_handle;
							fs_bulk_ports[server][incoming_port].net = net;
							fs_bulk_ports[server][incoming_port].stn = stn;
							fs_bulk_ports[server][incoming_port].ack_port = ack_port;
							fs_bulk_ports[server][incoming_port].length = length;
							fs_bulk_ports[server][incoming_port].received = 0; // Initialize
							fs_bulk_ports[server][incoming_port].reply_port = reply_port;
							fs_bulk_ports[server][incoming_port].rx_ctrl = rx_ctrl;
							fs_bulk_ports[server][incoming_port].mode = 3;
							fs_bulk_ports[server][incoming_port].user_handle = 0; // Rogue for no user handle, because never hand out user handle 0. This stops the bulk transfer routine trying to increment a cursor on a user handle which doesn't exist.
							fs_bulk_ports[server][incoming_port].last_receive = (unsigned long long) time(NULL);
						}
					}
				}
				else fs_error(server, reply_port, net, stn, 0xBD, "Insufficient access");


			}

		}
		else fs_error(server, reply_port, net, stn, 0xCC, "Bad path");
	}
	else
		fs_error(server, reply_port, net, stn, 0xC0, "Too many open files");
	
	
}

// Change ownership
void fs_free(int server, unsigned short reply_port, unsigned char net, unsigned char stn, int active_id, unsigned char *data, int datalen)
{

	struct __econet_packet_udp r;
	unsigned char path[1024];
	unsigned short disc;
	unsigned char discname[17], tmp[17];

	r.p.port = reply_port;
	r.p.ctrl = 0x80;
	r.p.ptype = ECONET_AUN_DATA;
	r.p.data[0] = r.p.data[1] = 0;

	fs_copy_to_cr(tmp, data+5, 16);
	snprintf((char * ) discname, 17, "%-16s", (const char * ) tmp);

	if (!fs_quiet) fprintf (stderr, "   FS:%12sfrom %3d.%3d Read free space on %s\n", "", net, stn, discname);

	disc = 0;
	while (disc < ECONET_MAX_FS_DISCS)
	{
		char realname[20];
		snprintf(realname, 17, "%-16s", (const char * ) fs_discs[server][disc].name);

		if (!strcasecmp((const char *) discname, (const char *) realname))
		{	
			struct statvfs s;

			snprintf((char * ) path, 1024, "%s/%1d%s",(const char * ) fs_stations[server].directory, disc, (const char * ) fs_discs[server][disc].name);
	
			if (!statvfs((const char * ) path, &s))
			{
				unsigned long long f; // free space
				unsigned long long e; // extent of filesystem

				f = (s.f_bsize >> 8) * s.f_bavail;
				e = (s.f_bsize >> 8) * s.f_blocks;

				// This is well dodgy and probably no use unless you put the filestore on a smaller filing system

				if (f > 0xffffff) f = 0xffffff;

				r.p.data[2] = (f % 256) & 0xff;
				r.p.data[3] = ((f >> 8) % 256) & 0xff;
				r.p.data[4] = ((f >> 16) % 256) & 0xff;

				if (e > 0xffffff) e = 0xffffff;

				r.p.data[5] = (e % 256) & 0xff;
				r.p.data[6] = ((e >> 8) % 256) & 0xff;
				r.p.data[7] = ((e >> 16) % 256) & 0xff;

				fs_aun_send(&r, server, 8, net, stn);
				return;

			}
			else fs_error(server, reply_port, net, stn, 0xFF, "FS Error");	
		}
		disc++;
	}
	
	fs_error(server, reply_port, net, stn, 0xFF, "No such disc");

	
}
// Return error specifying who owns a file
void fs_owner(int server, unsigned short reply_port, int active_id, unsigned char net, unsigned char stn, unsigned char *command)
{

	struct path p;
	unsigned char path[256];
	unsigned char result[30];
	unsigned char username[11];
	unsigned short ptr_file, ptr;

	fs_copy_to_cr(path, command, 1023);

	if (!fs_quiet) fprintf (stderr, "   FS:%12sfrom %3d.%3d *OWNER %s\n", "", net, stn, path);

	ptr = 0;

	while (*(command + ptr) == ' ' && ptr < strlen((const char *) command))
		ptr++;

	if (ptr == strlen((const char *) command))
		fs_error(server, reply_port, net, stn, 0xFE, "Bad command");

	ptr_file = ptr;

	while (*(command + ptr) != ' ' && ptr < strlen((const char *) command))
		ptr++;

	*(command + ptr) = '\0';

	strncpy((char * ) path, (const char * ) &(command[ptr_file]), 255);

	if (!fs_normalize_path(server, active_id, path, active[server][active_id].current, &p) || p.ftype == FS_FTYPE_NOTFOUND)
		fs_error(server, reply_port, net, stn, 0xD6, "Not found");
	else
	{
		if (!((active[server][active_id].priv & FS_PRIV_SYSTEM) || (p.owner == active[server][active_id].userid) || (p.parent_owner == active[server][active_id].userid))) // Not system user, and doesn't own parent directory
		{
			fs_error(server, reply_port, net, stn, 0xBD, "Insufficient access");
			return;
		}

		snprintf(username, 11, "%-10s", users[server][p.owner].username);
		snprintf(result, 30, "Owner: %-10s %04d", username, p.owner);

		fs_error(server, reply_port, net, stn, 0xFF, result);		

	}
}

// Change ownership
void fs_chown(int server, unsigned short reply_port, int active_id, unsigned char net, unsigned char stn, unsigned char *command)
{

	struct path p;
	unsigned char path[256];
	unsigned char username[20];
	unsigned short ptr_file, ptr_owner, ptr;
	int userid;

	fs_copy_to_cr(path, command, 1023);

	if (!fs_quiet) fprintf (stderr, "   FS:%12sfrom %3d.%3d *CHOWN %s\n", "", net, stn, path);

	userid = active[server][active_id].userid;

	ptr = 0;

	while (*(command + ptr) == ' ' && ptr < strlen((const char *) command))
		ptr++;

	if (ptr == strlen((const char *) command))
		fs_error(server, reply_port, net, stn, 0xFE, "Bad command");

	ptr_file = ptr;

	while (*(command + ptr) != ' ' && ptr < strlen((const char *) command))
		ptr++;

	if (ptr == strlen((const char *) command)) // No user specified - assume us
		ptr_owner = 0;
	else
	{
		*(command + ptr) = '\0';

		ptr++;
		while (*(command + ptr) == ' ' && ptr < strlen((const char *) command))
			ptr++;
		if (ptr == strlen((const char *) command)) // No user specified
			ptr_owner = 0;
		else	ptr_owner = ptr;

		while (*(command + ptr) != ' ' && ptr < strlen((const char *) command))
			ptr++; // Skip past owner name

		if (ptr < strlen((const char *) command))
			command[ptr] = '\0'; // Null terminate the username
	}

	strncpy((char * ) path, (const char * ) &(command[ptr_file]), 255);

	snprintf((char * ) username, 11, "%-10s", (ptr_owner ? (const char * ) &(command[ptr_owner]) : (const char * ) users[server][userid].username));
	
	username[10] = '\0';
	
	if (!fs_quiet) fprintf(stderr, "   FS:%12sfrom %3d.%3d Change ownership on %s to '%s'\n", "", net, stn, path, (char *) (ptr_owner ? (char *) username : (char *) "self"));

	if ((!(active[server][active_id].priv & FS_PRIV_SYSTEM)) && (ptr_owner != 0)) // Ordinary user tring to change ownership to someone other than themselves
	{
		fs_error(server, reply_port, net, stn, 0xBD, "Insufficient access");
		return;
	}

	if (!fs_normalize_path(server, active_id, path, active[server][active_id].current, &p) || p.ftype == FS_FTYPE_NOTFOUND)
		fs_error(server, reply_port, net, stn, 0xD6, "Not found");
	else
	{
		short newid, found;
		newid = 0;
		found = 0;
		
		if (ptr_owner == 0) 
		{
			newid = userid;
			found = 1;
		}

		while (newid < ECONET_MAX_FS_USERS && !found)
		{
			if (!strncasecmp((const char *) users[server][newid].username, (const char *) username, 10))
			{
				found = 1;
				break;
			}
			else newid++;
		}

		if (!found)
		{
			fs_error(server, reply_port, net, stn, 0xBC, "No such user");
			return;
		}

		// Now check we have permission

		if (p.perm & FS_PERM_L) // Locked
		{
			fs_error(server, reply_port, net, stn, 0xC3, "Locked");
			return;
		}

		if (
			!(active[server][active_id].priv & FS_PRIV_SYSTEM) &&
			(p.parent_owner == userid && !(p.parent_perm & FS_PERM_OWN_W)) &&
			!(p.owner == userid && (p.perm & FS_PERM_OWN_W))
		   ) // Not system user, no write access to parent directory
		{
			fs_error(server, reply_port, net, stn, 0xBD, "Insufficient access");
			return;
		}

		// normalize_path will have put the attributes in its attr struct - change & write to disc
		p.attr.owner = newid;

		fs_write_xattr(p.unixpath, p.attr.owner, p.attr.perm, p.attr.load, p.attr.exec);

		fs_reply_success(server, reply_port, net, stn, 0, 0);
		
	}
}

// Is a file open for reading or writing?
// This is the Econet locking mechanism.
// If >0 readers, can't open for writing.
// If a writer, can't open for reading.
// Supply the request type 1 for reading, 2 for writing (or deleting, renaming, etc.), 3 for writing and trucate
// Returns -3 for too many files, -1 for file didn't exist when it should or can't open, or internal handle for OK. This will also attempt to open the file 
// -2 = interlock failure
// The path is a unix path - we look it up in the tables of file handles
short fs_open_interlock(int server, unsigned char *path, unsigned short mode, unsigned short userid)
{

	unsigned short count;

	count = 0;

	while (count < ECONET_MAX_FS_FILES)
	{
		if (fs_files[server][count].handle && !strcmp(fs_files[server][count].name, path)) // Handle check ensures this is an active entry
		{
			if (mode >= 2) // We want write
				return -2; // If there is an active entry, someone must be reading or writing, so we can't write.
			else
				if (fs_files[server][count].writers == 0) // We can open this existing handle for reading
				{
					fs_files[server][count].readers++;
					return count; // Return the index into fs_files
				}
				else // We can't open for reading because someone else has it open for writing
					return -2;
		}
		else 	count++;
	}

	// If we've got here, then there is no existing handle for *path. Create one

	count = 0;

	while (count  < ECONET_MAX_FS_FILES)
	{
		if (fs_files[server][count].handle == NULL) // Empty descriptor
		{
			fs_files[server][count].handle = fopen(path, (mode == 1 ? "r" : (mode == 2 ? "r+" : "w+"))); // These correspond to OPENIN, OPENUP and OPENOUT. OPENUP can only be used if the file exists, so this line fails if it doesn't. Whereas w+ == OPENOUT, which can create a file.

			if (!fs_files[server][count].handle)
				return -1; // Failure
	
			strcpy(fs_files[server][count].name, path);
			if (mode == 1)	fs_files[server][count].readers = 1;
			else		fs_files[server][count].writers = 1;

			if (mode == 3) // Take ownereship on OPENOUT
				fs_write_xattr(path, userid, FS_PERM_OWN_W | FS_PERM_OWN_R, 0, 0);
	
			return count;
		}
		else count++;
	}

	// If we got here, then we couldn't find a spare descriptor - return 0

	return 0;

}

// Reduces the reader/writer count by 1 and, if both are 0, closes the file handle
void fs_close_interlock(int server, unsigned short index, unsigned short mode)
{
	if (mode == 1) // Reader close
		fs_files[server][index].readers--;
	else	fs_files[server][index].writers--;

	if (fs_files[server][index].readers <= 0 && fs_files[server][index].writers <= 0)
	{
		fclose(fs_files[server][index].handle);
		fs_files[server][index].handle = NULL; // Flag unused
	}

}

// Count how many existing directory entries in a directory
unsigned int fs_count_dir_entries(char *path)
{

	unsigned int count = 0;
	DIR *d;
	struct dirent *entry;

	d = opendir((const char *) path);

	if (!d) return 0;

	while ((entry = readdir(d)))
		if (entry->d_name[0] != '.') count++;

	return count;	

}

// Copy file(s)
void fs_copy(int server, unsigned short reply_port, int active_id, unsigned char net, unsigned char stn, unsigned char *command)
{

	char source[1024], destination[1024];
	struct path p_src, p_dst;
	struct path_entry *e;
	unsigned short to_copy, all_files;

	if (!fs_quiet) fprintf (stderr, "   FS:%12sfrom %3d.%3d COPY %s\n", "", net, stn, command);

	if (sscanf(command, "%s %s", source, destination) != 2)
	{
		fs_error(server, reply_port, net, stn, 0xFF, "Bad parameters");
		return;
	}

	if (!fs_normalize_path_wildcard(server, active_id, source, active[server][active_id].current, &p_src, 1))
	{
		fs_error(server, reply_port, net, stn, 0xDC, "Not found");
		fs_free_wildcard_list(&p_src);
		return;
	}
	
	all_files = 0;
	to_copy = 0;

	// Check they're all files

	e = p_src.paths;

	while (e != NULL)
	{
		if (e->ftype == FS_FTYPE_FILE && (e->my_perm & FS_PERM_OWN_R)) all_files++;
		to_copy++;
		e = e->next;
	}

	if (all_files != to_copy) // Not all files! Error
	{
		fs_error(server, reply_port, net, stn, 0xFF, "Source must be all files");
		fs_free_wildcard_list(&p_src);
		return;
	}

	// Make sure destination is a directory (unless only one file to copy)

	if (!fs_normalize_path(server, active_id, destination, active[server][active_id].current, &p_dst))
	{
		fs_error(server, reply_port, net, stn, 0xFF, "Bad destination");
		fs_free_wildcard_list(&p_src);
		return;
	}

	if (p_dst.ftype != FS_FTYPE_DIR && to_copy > 1) // Can't copy > 1 file into something not a directory
	{
		fs_error(server, reply_port, net, stn, 0xFF, "Destination not a dir");
		fs_free_wildcard_list(&p_src);
		return;
	}

	e = p_src.paths; // Copy them

	while (e != NULL)
	{

		short handle, out_handle;
		struct objattr a;
		unsigned long length, sf_return;
		off_t readpos;
		char destfile[1048];

		handle = fs_open_interlock(server, e->unixpath, 1, active[server][active_id].userid);

		//fprintf (stderr, "fs_open_interlock(%s) returned %d\n", e->unixpath, handle);

		if (handle == -3)
		{
			fs_error(server, reply_port, net, stn, 0xC0, "Too many open files");
			fs_free_wildcard_list(&p_src);
			return;
		}
		else if (handle == -2)
		{
			fs_error(server, reply_port, net, stn, 0xC2, "Already open");
			fs_free_wildcard_list(&p_src);
			return;
		}
		else if (handle == -1)
		{
			fs_error(server, reply_port, net, stn, 0xFF, "FS Error");
			fs_free_wildcard_list(&p_src);
			return;
		}

		fs_read_xattr(e->unixpath, &a);

		if (p_dst.ftype == FS_FTYPE_DIR)
			sprintf(destfile, "%s/%s", p_dst.unixpath, e->unixfname);
		else
			strcpy(destfile, p_dst.unixpath); 

		out_handle = fs_open_interlock(server, destfile, 3, active[server][active_id].userid);

		//fprintf (stderr, "fs_open_interlock(%s) returned %d\n", destfile, out_handle);

		if (out_handle == -3)
		{
			fs_error(server, reply_port, net, stn, 0xC0, "Too many open files");
			fs_free_wildcard_list(&p_src);
			return;
		}
		else if (out_handle == -2) // Should never happen
		{
			fs_error(server, reply_port, net, stn, 0xC2, "Already open");
			fs_free_wildcard_list(&p_src);
			return;
		}
		else if (out_handle == -1)
		{
			fs_error(server, reply_port, net, stn, 0xFF, "FS Error");
			fs_free_wildcard_list(&p_src);
			return;
		}

		fseek(fs_files[server][handle].handle, 0, SEEK_END);
		length = ftell(fs_files[server][handle].handle);

		if (!fs_quiet) fprintf (stderr, "   FS:%12sfrom %3d.%3d Copying %s to %s, length %06lX\n", "", net, stn, e->unixpath, destfile, length);

		readpos = 0; // Start at the start

		while (readpos < length)
		{
			if ((sf_return = sendfile(fileno(fs_files[server][out_handle].handle),
				fileno(fs_files[server][handle].handle),
				&readpos, 
				length)) == -1) // Error!
			{
				fs_close_interlock(server, handle, 1);
				fs_close_interlock(server, out_handle, 3);
				fs_free_wildcard_list(&p_src);
				fs_error(server, reply_port, net, stn, 0xFF, "FS Error in copy");
				return;
			}

			readpos += sf_return;
		}

		fs_write_xattr(destfile, active[server][active_id].userid, a.perm, a.load, a.exec);
		fs_close_interlock(server, handle, 1);
		fs_close_interlock(server, out_handle, 3);

		e = e->next;
	}

	fs_free_wildcard_list(&p_src);
	fs_reply_ok(server, reply_port, net, stn);

}

// System command - create symbolic link (e.g. "duplicate" library)
void fs_link(int server, unsigned short reply_port, int active_id, unsigned char net, unsigned char stn, unsigned char *command)
{

	char source[1024], destination[1024];
	struct path p_src, p_dst;

	if (!fs_quiet) fprintf (stderr, "   FS:%12sfrom %3d.%3d LINK %s\n", "", net, stn, command);
	if (sscanf(command, "%s %s", source, destination) != 2)
	{
		fs_error(server, reply_port, net, stn, 0xFF, "Bad parameters");
		return;
	}

	if (!fs_normalize_path(server, active_id, source, active[server][active_id].current, &p_src) || (p_src.ftype == FS_FTYPE_NOTFOUND))
	{
		fs_error(server, reply_port, net, stn, 0xDC, "Not found");
		fs_free_wildcard_list(&p_src);
		return;
	}
	
	if (!fs_normalize_path(server, active_id, destination, active[server][active_id].current, &p_dst))
	{
		fs_error(server, reply_port, net, stn, 0xDC, "Bad destination path");
		fs_free_wildcard_list(&p_src);
		fs_free_wildcard_list(&p_dst);
		return;
	}
	
	//fprintf (stderr, "Calling symlink(%s, %s)\n", p_src.unixpath, p_dst.unixpath);

	if (symlink(p_src.unixpath, p_dst.unixpath) == -1)
	{
		fs_error(server, reply_port, net, stn, 0xFF, "Cannot create link");
		fs_free_wildcard_list(&p_src);
		fs_free_wildcard_list(&p_dst);
		return;
	}
	
	fs_write_xattr(p_src.unixpath, p_src.owner, p_src.perm | FS_PERM_L, p_src.load, p_src.exec); // Lock the file. If you remove the file to which there are symlinks, stat goes bonkers and the FS crashes. So lock the source file so the user has to think about it!! (Obviously this will show as a locked linked file too, but hey ho)

	fs_free_wildcard_list(&p_src);
	fs_free_wildcard_list(&p_dst);

	fs_reply_ok(server, reply_port, net, stn);

}

// Select other disc
void fs_sdisc(int server, unsigned short reply_port, int active_id, unsigned char net, unsigned char stn, unsigned char *command)
{

	// Collect new directory handles, and only if they're all good are we going to switch discs

	int root, cur, lib;
	struct path p_root, p_home, p_lib;
	char discname[20];
	char tmppath[1024];
	int internal_root_handle, internal_cur_handle, internal_lib_handle;
	unsigned char home_dir[100], lib_dir[100];

	struct __econet_packet_udp r;

	fs_copy_to_cr(discname, command, 19);

	root = cur = lib = -1;

	strncpy((char *) home_dir, (const char *) users[server][active[server][active_id].userid].home, 96);
	home_dir[96] = '\0';

	strncpy((char *) lib_dir, (const char *) users[server][active[server][active_id].userid].lib, 96);
	lib_dir[96] = '\0';

	// Root directory first

	sprintf(tmppath, ":%s.$", discname);

	if (!fs_quiet) fprintf(stderr, "   FS:%12sfrom %3d.%3d Change disc to %s\n", "", net, stn, discname);

	if (!fs_normalize_path(server, active_id, tmppath, -1, &p_root))
	{
		fs_error(server, reply_port, net, stn, 0xFF, "Cannot map root directory on new disc");
		return;
	}

	if (p_root.ftype != FS_FTYPE_DIR)
	{
		fs_error(server, reply_port, net, stn, 0xFF, "Cannot map root directory on new disc");
		return;
	}

	if ((internal_root_handle = fs_get_dir_handle(server, active_id, p_root.unixpath)) == -1)
	{
		fs_error(server, reply_port, net, stn, 0xFF, "Root directory inaccessible!");
		return;
	}

	if ((root = fs_allocate_user_dir_channel(server, active_id, internal_root_handle)) == -1) // Can't allocate handle
	{
		fs_error(server, reply_port, net, stn, 0xFF, "Root directory channel ?");
		fs_close_dir_handle(server, internal_root_handle);
		return;
	}

	strcpy(active[server][active_id].fhandles[root].acornfullpath, p_root.acornfullpath);

	if (!fs_quiet) fprintf(stderr, "   FS:%12sfrom%3d.%3d Successfully mapped new root - handle %02X, full path %s\n", "", net, stn, root, active[server][active_id].fhandles[root].acornfullpath);

	sprintf(tmppath, ":%s.%s", discname, home_dir);
	if (!fs_quiet) fprintf(stderr, "   FS%12s from %3d.%3d Attempting to find home dir %s\n", "", net, stn, tmppath);

        if (!fs_normalize_path(server, active_id, tmppath, -1, &p_home))
        {
		internal_cur_handle = internal_root_handle;
        }
	else if (p_home.ftype == FS_FTYPE_NOTFOUND || p_home.disc != users[server][active[server][active_id].userid].home_disc) // Not on home disc - use root as CWD
	{
		//fprintf (stderr, "p_home.disc = %d, home disc = %d\n", p_home.disc, users[server][active[server][active_id].userid].home_disc);
		internal_cur_handle = internal_root_handle;
	}
	else
	{
        	if (p_home.ftype != FS_FTYPE_DIR)
        	{
                	fs_error(server, reply_port, net, stn, 0xFF, "Cannot map home directory on new disc");
                	return;
        	}
	
        	if ((internal_cur_handle = fs_get_dir_handle(server, active_id, p_home.unixpath)) == -1)
        	{
                	fs_error(server, reply_port, net, stn, 0xFF, "Home directory inaccessible!");
                	return;
        	}
	
		fprintf(stderr, "New home internal handle %d\n", internal_cur_handle);
	}

       	if ((cur = fs_allocate_user_dir_channel(server, active_id, internal_cur_handle)) == -1) // Can't allocate handle
       	{
               	fs_error(server, reply_port, net, stn, 0xFF, "Home directory channel ?");
		fs_deallocate_user_dir_channel(server, active_id, root);
               	fs_close_dir_handle(server, internal_cur_handle);
       	       	return;
	}
        
	strcpy(active[server][active_id].fhandles[cur].acornfullpath, p_home.acornfullpath);

	if (!fs_quiet) fprintf(stderr, "   FS:%12sfrom%3d.%3d Successfully mapped new CWD - handle %02X, full path %s\n", "", net, stn, cur, active[server][active_id].fhandles[cur].acornfullpath);

	sprintf(tmppath, ":%s.%s", discname, lib_dir);

        if (!fs_normalize_path(server, active_id, tmppath, -1, &p_lib) || p_lib.ftype == FS_FTYPE_NOTFOUND)
        {
                // Map lib to root!
		internal_lib_handle = internal_root_handle;
        }
        else
        {
                if (p_lib.ftype != FS_FTYPE_DIR)
                {
                        fs_error(server, reply_port, net, stn, 0xFF, "Cannot map library directory on new disc");
                        return;
                }

                if ((internal_lib_handle = fs_get_dir_handle(server, active_id, p_lib.unixpath)) == -1)
                {
                        fs_error(server, reply_port, net, stn, 0xFF, "Library directory inaccessible!");
                        return;
                }

	}

        if ((lib = fs_allocate_user_dir_channel(server, active_id, internal_lib_handle)) == -1) // Can't allocate handle
        {
                fs_error(server, reply_port, net, stn, 0xFF, "Library directory channel ?");
		fs_deallocate_user_dir_channel(server, active_id, root);
		fs_deallocate_user_dir_channel(server, active_id, cur);
                fs_close_dir_handle(server, internal_lib_handle);
                return;
        }

	if (!fs_quiet) fprintf(stderr, "   FS:%12sfrom%3d.%3d Successfully mapped new Library - handle %02X, full path %s\n", "", net, stn, lib, active[server][active_id].fhandles[lib].acornfullpath);

	strcpy(active[server][active_id].fhandles[lib].acornfullpath, p_lib.acornfullpath);

	// Got here, so new disc selection has worked - TODO - release old handles and update active structure, including tail entries.

	//if (!fs_quiet) fprintf(stderr, "   FS:%12sfrom%3d.%3d Attempting to deallocate handles %d, %d, %d\n", "", net, stn, active[server][active_id].root, active[server][active_id].current, active[server][active_id].lib);
	
	fs_deallocate_user_dir_channel(server, active_id, active[server][active_id].lib);
	fs_deallocate_user_dir_channel(server, active_id, active[server][active_id].root);
	fs_deallocate_user_dir_channel(server, active_id, active[server][active_id].current);
	
	active[server][active_id].lib = lib;
	active[server][active_id].current = cur;
	active[server][active_id].root = root;
	active[server][active_id].current_disc = p_root.disc;

	strncpy((char *) active[server][active_id].root_dir, (const char *) "", 11);
	strncpy((char *) active[server][active_id].root_dir_tail, (const char *) "$         ", 11);

	if (internal_cur_handle != internal_root_handle)
	{
		strncpy((char *) active[server][active_id].current_dir, (const char *) p_home.path_from_root, 255);
		if (p_home.npath == 0)
			sprintf(active[server][active_id].current_dir_tail, "$         ");
		else	sprintf(active[server][active_id].current_dir_tail, "%-10s", p_home.path[p_home.npath-1]);
	}
	else
	{
		strncpy((char *) active[server][active_id].current_dir, (const char *) "", 11);
		strncpy((char *) active[server][active_id].current_dir_tail, (const char *) "$         ", 11);
	}	

	if (internal_lib_handle != internal_root_handle)
	{
        	strncpy((char *) active[server][active_id].lib_dir, (const char *) p_lib.path_from_root, 255);
        	
		if (p_lib.npath == 0) 
			sprintf(active[server][active_id].lib_dir_tail, "$         ");
        	else    sprintf(active[server][active_id].lib_dir_tail, "%-10s", p_lib.path[p_lib.npath-1]);
	}
	else
	{
		strncpy((char *) active[server][active_id].lib_dir, (const char *) "", 11);
		strncpy((char *) active[server][active_id].lib_dir_tail, (const char *) "$         ", 11);
	}	
	
	active[server][active_id].lib_dir_tail[10] = '\0';
	active[server][active_id].current_dir_tail[10] = '\0';
	active[server][active_id].root_dir_tail[10] = '\0';

	if (!fs_quiet) fprintf (stderr, "   FS:%12sfrom %3d.%3d New (root, current, lib) = (%s, %s, %s)\n", "", net, stn, active[server][active_id].root_dir, active[server][active_id].current_dir, active[server][active_id].lib_dir);

	r.p.ptype = ECONET_AUN_DATA;
	r.p.port = reply_port;
	r.p.ctrl = 0x80;
	r.p.data[0] = 0x06; // SDisc return, according to MDFS manual
	r.p.data[1] = 0x00;
	r.p.data[2] = root;
	r.p.data[3] = cur;
	r.p.data[4] = lib;
	r.p.data[5] = active[server][active_id].bootopt;

	fs_aun_send(&r, server, 6, net, stn);

}

// Rename a file (i.e. move it)
void fs_rename(int server, unsigned short reply_port, int active_id, unsigned char net, unsigned char stn, int relative_to, unsigned char *command)
{

	struct path p_from, p_to;
	unsigned char from_path[1024], to_path[1024];
	unsigned short count, found;
	unsigned short firstpath_start, firstpath_end, secondpath_start, secondpath_end;
	short handle;
	struct __econet_packet_udp r;

	count = found = 0;

	// First, find the source path

	while (!found && (count < strlen((const char *) command)))
	{
		if (*(command + count) != ' ') found = 1;
		else count++;
	}

	if (count == strlen((const char *) command))
	{
		fs_error(server, reply_port, net, stn, 0xFD, "Bad string");
		return;
	}

	firstpath_start = count;

	found = 0;

	while (!found && (count < strlen((const char *) command)))
	{
		if (*(command + count) == ' ') found = 1;
		else count++;
	}

	if (count == strlen((const char *) command)) // Ran out without finding some space separating first string from second
	{
		fs_error(server, reply_port, net, stn, 0xFD, "Bad string");
		return;
	}

	firstpath_end = count-1;

	found = 0;

	while (!found && (count < strlen((const char *) command)))
	{
		if (*(command + count) != ' ') found = 1; // Found start of second path
		else count++;
	}

	if (count == strlen((const char *) command))
	{
		fs_error(server, reply_port, net, stn, 0xFD, "Bad string");
		return;
	}

	secondpath_start = count;

	found = 0;

	while (!found && (count < strlen((const char *) command)))
	{
		if (*(command + count) == ' ') found = 1;
		else count++;
	}

	secondpath_end = count-1;

	strncpy(from_path, (command+firstpath_start), (firstpath_end - firstpath_start + 1));
	from_path[(firstpath_end - firstpath_start + 1)] = '\0';

	strncpy(to_path, (command+secondpath_start), (secondpath_end - secondpath_start + 1));
	to_path[(secondpath_end - secondpath_start + 1)] = '\0';

	if (!fs_quiet) fprintf(stderr, "   FS:%12sfrom %3d.%3d Rename from %s to %s\n", "", net, stn, from_path, to_path);	

	if (!fs_normalize_path(server, active_id, from_path, active[server][active_id].current, &p_from) || !fs_normalize_path(server, active_id, to_path, active[server][active_id].current, &p_to) || p_from.ftype == FS_FTYPE_NOTFOUND)
	{
		fs_error(server, reply_port, net, stn, 0xDC, "Not found");
		return;
	}

	//fprintf (stderr, "Rename parms: from locked: %s, from_owner %04x, from_parent_owner %04x, to_ftype %02x, to_owner %04x, to_parent_owner %04x, to_perm %02x, to_parent_perm %02x\n", 
			//(p_from.perm & FS_PERM_L ? "Yes" : "No"), p_from.owner, p_from.parent_owner, p_to.ftype, p_to.owner, p_to.parent_owner, p_to.perm, p_to.parent_perm);

	if (p_from.perm & FS_PERM_L) // Source locked
	{
		fs_error(server, reply_port, net, stn, 0xC3, "Entry locked");
		return;
	}
	
	if ((p_from.owner != active[server][active_id].userid) && (p_from.parent_owner != active[server][active_id].userid) && ((active[server][active_id].priv & FS_PRIV_SYSTEM) == 0))
	{
		fs_error(server, reply_port, net, stn, 0xBD, "Insufficient access");
		return;
	}

	if ((p_to.ftype != FS_FTYPE_NOTFOUND) && p_to.ftype != FS_FTYPE_DIR) // I.e. destination does exist but isn't a directory - cannot move anything on top of existing file
	{
		fs_error(server, reply_port, net, stn, 0xFF, "Destination exists");
		return;
		// Note, we *can* move a file into a filename inside a directory (FS_FTYPE_NOTFOUND), likewise a directory, but if the destination exists it MUST be a directory
	}

	if ((p_to.ftype == FS_FTYPE_NOTFOUND) && p_to.parent_owner != active[server][active_id].userid && ((p_to.parent_perm & FS_PERM_OTH_W) == 0)) // Attempt to move to a directory we don't own and don't have write access to
	{
		fs_error(server, reply_port, net, stn, 0xBD, "Insufficient access");
		return;
	}

	if ((p_to.ftype != FS_FTYPE_NOTFOUND && p_to.owner != active[server][active_id].userid && (active[server][active_id].priv & FS_PRIV_SYSTEM) == 0)) // Destination exists (so must be dir), not owned by us, and we're not system
	{
		fs_error(server, reply_port, net, stn, 0xBD, "Insufficient access");
		return;
	}

	// Get an interlock

	if (p_from.ftype == FS_FTYPE_FILE)
	{
		handle = fs_open_interlock(server, p_from.unixpath, 2, active[server][active_id].userid);
	
		switch (handle)
		{
			case -1: // Can't open
			{
				fprintf(stderr, "fs_open_interlock() returned -1");
				fs_error(server, reply_port, net, stn, 0xFF, "FS Error");
				return;
			}
			break;
			case -2: // Interlock failure
			{
				fs_error(server, reply_port, net, stn, 0xC2, "Already open");
				return;
			}
			break;
			case -3: // Too many files
			{
				fs_error(server, reply_port, net, stn, 0xC0, "Too many open files");
				return;
			}
			break;
		}

		// Release the interlock (since nothing else is going to come along and diddle with the file in the meantime

		fs_close_interlock(server, handle, 3);
	}


	// Otherwise we should be able to move it... and Unlike Econet, we *can* move across "discs"

#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1 << 0)
#endif

	if (syscall(SYS_renameat2, 0, p_from.unixpath, 0, p_to.unixpath, RENAME_NOREPLACE)) // non-zero - failure
	{
		fs_error(server, reply_port, net, stn, 0xFF, "FS Error");
		return;
	}

	r.p.ptype = ECONET_AUN_DATA;
	r.p.port = reply_port;
	r.p.ctrl = 0x80;
	r.p.data[0] = r.p.data[1] = 0;

	fs_aun_send (&r, server, 2, net, stn);

}
	

// Delete a file
void fs_delete(int server, unsigned short reply_port, int active_id, unsigned char net, unsigned char stn, int relative_to, unsigned char *command)
{

	struct path p;
	unsigned char path[1024];
	unsigned short count, found;
	short handle;

	count = found = 0;

	while (!found && (count < strlen((const char *) command)))
	{
		if (*(command+count) != ' ') found = 1;
		else count++;
	}

	fs_copy_to_cr(path, command + count, 1023);

	if (!fs_normalize_path_wildcard(server, active_id, path, relative_to, &p, 1))
		fs_error(server, reply_port, net, stn, 0xd6, "Not found");
	else
	{

		struct path_entry *e;

		e = p.paths;

		while (e != NULL) // Cycle through the entries
		{
			if (e->ftype == FS_FTYPE_FILE)
			{
				handle = fs_open_interlock(server, e->unixpath, 2, active[server][active_id].userid);
		
				if (handle < 0) // Interlock or other problem
				{
					fs_error(server, reply_port, net, stn, 0xc2, "Already open");
					fs_free_wildcard_list(&p);
					return;
				}
				else	fs_close_interlock(server, handle, 2);
			}
		
			if (e->ftype == FS_FTYPE_DIR && (fs_get_acorn_entries(server, active_id, p.unixpath) > 0))
			{
				fs_free_wildcard_list(&p);
				fs_error(server, reply_port, net, stn, 0xff, "Dir not empty");
				return;
			}
			else if (p.ftype == FS_FTYPE_NOTFOUND)
			{
				fs_free_wildcard_list(&p);
				fs_error(server, reply_port, net, stn, 0xd6, "Not found");
				return;
			}
			else if ((e->perm & FS_PERM_L))
			{
				fs_free_wildcard_list(&p);
				fs_error(server, reply_port, net, stn, 0xC3, "Entry locked");
				return;
			}
			else if (
					!(	(e->owner == active[server][active_id].userid) || ((e->parent_owner == active[server][active_id].userid) && (e->parent_perm & FS_PERM_OWN_W))
				)
			)
			{
				fs_free_wildcard_list(&p);
				fs_error(server, reply_port, net, stn, 0xBD, "Insufficient access");
				return;
			}
			else
			if ( 
					((e->ftype == FS_FTYPE_FILE) && unlink((const char *) e->unixpath)) ||
				((e->ftype == FS_FTYPE_DIR) && rmdir((const char *) e->unixpath))
		   		) // Failed
				{	if (!fs_quiet) fprintf (stderr, "   FS:%12sfrom %3d.%3d Failed to unlink %s\n", "", net, stn, e->unixpath);
					fs_free_wildcard_list(&p);
					fs_error(server, reply_port, net, stn, 0xFF, "FS Error");
					return;
				}
		
			e = e->next;
		}

		fs_reply_success(server, reply_port, net, stn, 0, 0);
	}

}

// Create directory
void fs_cdir(int server, unsigned short reply_port, int active_id, unsigned char net, unsigned char stn, int relative_to, unsigned char *command)
{

	struct path p;
	unsigned char path[1024];
	unsigned short count, found;

	count = found = 0;

	while (!found && (count < strlen((const char *) command)))
	{
		if (*(command+count) != ' ') found = 1;
		else count++;
	}

	fs_copy_to_cr(path, command + count, 1023);

	if (!fs_normalize_path(server, active_id, path, relative_to, &p))
		fs_error(server, reply_port, net, stn, 0xD6, "Not found");
	else
	{

		if (p.ftype != FS_FTYPE_NOTFOUND)
			fs_error(server, reply_port, net, stn, 0xFF, "Exists");
		else if ((p.parent_owner == active[server][active_id].userid && (p.parent_perm & FS_PERM_OWN_W)) || (users[server][active[server][active_id].userid].priv & FS_PRIV_SYSTEM)) // Must own the parent and have write access, or be system
		{
			if (!mkdir((const char *) p.unixpath, 0770))
			{
				fs_write_xattr(p.unixpath, active[server][active_id].userid, FS_PERM_OWN_W | FS_PERM_OWN_R, 0, 0);
				fs_reply_success(server, reply_port, net, stn, 0, 0);
			}
			else	fs_error(server, reply_port, net, stn, 0xFF, "Unable to make directory");
		}
		else fs_error(server, reply_port, net, stn, 0xBD, "Insufficient access");
	}
	
}

void fs_info(int server, unsigned short reply_port, int active_id, unsigned char net, unsigned char stn, unsigned char *command)
{

	struct path p;
	struct __econet_packet_udp r;

	unsigned char path[1024];
	unsigned char relative_to;
	char reply_string[81];

	r.p.port = reply_port;
	r.p.ctrl = 0x80;
	r.p.ptype = ECONET_AUN_DATA;

	fs_copy_to_cr(path, command, 1023);

	relative_to = active[server][active_id].current;

	//r.p.data[0] = relative_to; Maybe this is a permissions thing?
	r.p.data[0] = 0x04; // Anything else and we get weird results. 0x05, for example, causes the client machine to *RUN the file immediately after getting the answer...
	r.p.data[1] = 0;

	if (!fs_quiet) fprintf (stderr, "   FS:%12sfrom %3d.%3d *INFO %s\n", "", net, stn, path);

	if (!fs_normalize_path(server, active_id, path, relative_to, &p))
		fs_error(server, reply_port, net, stn, 0xD6, "Not found");
	else
	{
		if (p.ftype == FS_FTYPE_NOTFOUND)
			fs_error(server, reply_port, net, stn, 0xD6, "Not found");
		else if (p.ftype != FS_FTYPE_FILE)
			fs_error(server, reply_port, net, stn, 0xD6, "Not a file");
		else if (p.owner != active[server][active_id].userid && (p.perm & FS_PERM_H)) // Hidden file
			fs_error(server, reply_port, net, stn, 0xD6, "Not found");
		else
		{
			unsigned char permstring[10];

			strcpy(permstring, "");
		
			if (p.perm & FS_PERM_L) strcat (permstring, "L");
			if (p.perm & FS_PERM_OWN_W) strcat (permstring, "W");
			if (p.perm & FS_PERM_OWN_R) strcat (permstring, "R");
			strcat (permstring, "/");
			if (p.perm & FS_PERM_OTH_W) strcat (permstring, "W");
			if (p.perm & FS_PERM_OTH_R) strcat (permstring, "R");

			sprintf(reply_string, "%-10s %08lX %08lX   %06lX    %-7s   %02d/%02d/%02d %06lX%c%c",	p.path[p.npath-1], p.load, p.exec, p.length, permstring, 
					fs_day_from_two_bytes(p.day, p.monthyear),
					fs_month_from_two_bytes(p.day, p.monthyear),
					fs_year_from_two_bytes(p.day, p.monthyear),
					p.internal, 0x0d, 0x80);

			strcpy(&(r.p.data[2]), reply_string);
	
			fs_aun_send(&r, server, strlen(reply_string)+2, net, stn);
		}

	}

}


// Change permissions
void fs_access(int server, unsigned short reply_port, int active_id, unsigned char net, unsigned char stn, unsigned char *command)
{

	struct path p;
	unsigned char path[1024];
	unsigned char perm;
	unsigned short path_ptr;
	unsigned short ptr;

	fs_copy_to_cr(path, command, 1023);

	if (!fs_quiet) fprintf (stderr, "   FS:%12sfrom %3d.%3d *ACCESS %s\n", "", net, stn, path);

	ptr = 0;

	while (ptr < strlen((const char *) command) && *(command+ptr) == ' ')
		ptr++;

	if (ptr == strlen((const char *) command)) // No filespec
	{
		fs_error(server, reply_port, net, stn, 0xFC, "Bad file name");
		return;
	}

	path_ptr = ptr;

	while (ptr < strlen((const char *) command) && *(command+ptr) != ' ')
		ptr++;

	if (ptr == strlen((const char *) command)) // No access string given
	{
		fs_error(server, reply_port, net, stn, 0xCF, "Bad attribute");
		return;
	}

	//fprintf (stderr, "Command: %s, path_ptr = %d, ptr = %d\n", command, path_ptr, ptr);

	strncpy((char * ) path, (const char * ) command + path_ptr, (ptr - path_ptr));

	path[ptr - path_ptr] = '\0'; // Terminate the path

	ptr++;

	while (ptr < strlen((const char *) command) && *(command+ptr) == ' ') // Skip spaces again
		ptr++;

	if (ptr == strlen((const char *) command)) // No access string given
	{
		fs_error(server, reply_port, net, stn, 0xCF, "Bad attribute");
		return;
	}

	perm = 0;

	while (ptr < strlen((const char *) command) && *(command+ptr) != '/')
	{
		switch (*(command+ptr))
		{
			case 'W': perm |= FS_PERM_OWN_W; break;
			case 'R': perm |= FS_PERM_OWN_R; break;
			case 'H': perm |= FS_PERM_H; break; // Hidden from directory listings
			case 'L': perm |= FS_PERM_L; break; // Locked
			default:
			{
				fs_error(server, reply_port, net, stn, 0xCF, "Bad attribute");
				return;
			}
		}
		ptr++;

	}

	if (ptr != strlen((const char *) command))
	{
		ptr++; // Skip the '/'

		while (ptr < strlen((const char *) command) && (*(command+ptr) != ' ')) // Skip trailing spaces too
		{
			switch (*(command+ptr))
			{
				case 'W': perm |= FS_PERM_OTH_W; break;
				case 'R': perm |= FS_PERM_OTH_R; break;
				default: 
				{
					fs_error(server, reply_port, net, stn, 0xCF, "Bad attribute");
					return;
				}
			}
			ptr++;
		}
	}
			
	// Normalize the path

	if (!fs_normalize_path(server, active_id, path, active[server][active_id].current, &p))
	{
		fs_error(server, reply_port, net, stn, 0xD6, "Not found");
		return;
	}
	
	if (p.ftype == FS_FTYPE_NOTFOUND)
	{
		fs_error(server, reply_port, net, stn, 0xD6, "Not found");
		return;
	}

	if (p.attr.owner == active[server][active_id].userid || (p.parent_owner == active[server][active_id].userid && (p.parent_perm & FS_PERM_OWN_W)) || (users[server][active[server][active_id].userid].priv & FS_PRIV_SYSTEM)) // Must own the file, own the parent and have write access, or be system
	{
		p.attr.perm = perm;
	
		fs_write_xattr(p.unixpath, p.attr.owner, p.attr.perm, p.attr.load, p.attr.exec);
	
		fs_reply_success(server, reply_port, net, stn, 0, 0);
	}
	else
		fs_error(server, reply_port, net, stn, 0xBD, "Insufficient access");
}

// Read discs
void fs_read_discs(int server, unsigned short reply_port, unsigned char net, unsigned char stn, int active_id, unsigned char *data, int datalen)
{

	struct __econet_packet_udp r;

	unsigned short start = *(data+5);
	unsigned short number = *(data+6);
	unsigned short delivered = 0;
	unsigned short disc_ptr = 0;
	unsigned short found = 0;
	unsigned short data_ptr = 3;

	r.p.port = reply_port;
	r.p.ctrl = 0x80;
	r.p.ptype = ECONET_AUN_DATA;

	r.p.data[0] = 10;
	r.p.data[1] = 0;
	
	if (!fs_quiet) fprintf (stderr, "   FS:%12sfrom %3d.%3d Read Discs from %d (up to %d)\n", "", net, stn, start, number);

	while (disc_ptr < ECONET_MAX_FS_DISCS && found < start)
	{
		if (fs_discs[server][disc_ptr].name[0] != '\0') // Found an active disc
			found++;
		disc_ptr++;
	}

	if (disc_ptr < ECONET_MAX_FS_DISCS) // See if there are any to insert
	{
		while (disc_ptr < ECONET_MAX_FS_DISCS && (delivered < number))
		{
			if (fs_discs[server][disc_ptr].name[0] != '\0')
			{
				found++;	
				snprintf((char * ) &(r.p.data[data_ptr]), 18, "%c%-16s", disc_ptr, fs_discs[server][disc_ptr].name);
				delivered++;
				data_ptr += 17;
			}
			disc_ptr++;
		}
	}

	r.p.data[2] = delivered;

	fs_aun_send(&r, server, data_ptr, net, stn);

}

// Read time
void fs_read_time(int server, unsigned short reply_port, unsigned char net, unsigned char stn, int active_id, unsigned char *data, int datalen)
{

	struct __econet_packet_udp r;

	struct tm t;
	time_t now;
	unsigned char monthyear, day;

	if (!fs_quiet) fprintf (stderr, "   FS:%12sfrom %3d.%3d Read FS time\n", "", net, stn);

	now = time(NULL);
	t = *localtime(&now);

	fs_date_to_two_bytes(t.tm_mday, t.tm_mon+1, t.tm_year, &monthyear, &day);
	//monthyear = (((t.tm_year - 81 - 40) & 0x0f) << 4) | ((t.tm_mon+1) & 0x0f);	
	
	r.p.ptype = ECONET_AUN_DATA;
	r.p.port = reply_port;
	r.p.ctrl = 0x80;
	r.p.data[0] = r.p.data[1] = 0;

	r.p.data[2] = day;
	r.p.data[3] = monthyear;
	r.p.data[4] = t.tm_hour;
	r.p.data[5] = t.tm_min;
	r.p.data[6] = t.tm_sec;

	fs_aun_send(&r, server, 7, net, stn);

}

// Read logged on users
void fs_read_logged_on_users(int server, unsigned short reply_port, unsigned char net, unsigned char stn, int active_id, unsigned char *data, int datalen)
{

	struct __econet_packet_udp r;
	unsigned short start, number;
	unsigned short found;
	unsigned short active_ptr;
	unsigned short ptr;

	start = *(data+5);
	number = *(data+6);

	r.p.ptype = ECONET_AUN_DATA;
	r.p.port = reply_port;
	r.p.ctrl = 0x80;

	r.p.data[0] = r.p.data[1] = 0;
	r.p.data[2] = 0; // 0 users found unless we alter it later

	ptr = 3;
	
	if (!fs_quiet) fprintf (stderr, "   FS:%12sfrom %3d.%3d Read logged on users\n", "", net, stn);

	// Get to the start entry in active[server][]

	active_ptr = 0;
	found = 0;

	while (active_ptr < ECONET_MAX_FS_USERS && found < start)
	{
		if (active[server][active_ptr].net != 0 || active[server][active_ptr].stn != 0)
			found++;
		active_ptr++;
	}

	if (active_ptr < ECONET_MAX_FS_USERS) // We've found the first one the station wants
	{
		int deliver_count = 0;

		while (active_ptr < ECONET_MAX_FS_USERS && deliver_count < number)
		{
			if (active[server][active_ptr].net != 0 || active[server][active_ptr].stn != 0)
			{
				char username[11];
				strncpy((char * ) username, (const char * ) users[server][active[server][active_ptr].userid].username, 10);
				found++;
				deliver_count++;
				sprintf((char * ) &(r.p.data[ptr]), "%c%c%-10s%c", 
					active[server][active_ptr].stn, active[server][active_ptr].net,
					username,
					(active[server][active_ptr].priv & FS_PRIV_SYSTEM ? 1 : 0) );

				ptr +=13;
			}

			active_ptr++;
		}	

		r.p.data[2] = deliver_count;
	}


	fs_aun_send (&r, server, ptr, net, stn);
}

// Read user information
void fs_read_user_info(int server, unsigned short reply_port, unsigned char net, unsigned char stn, int active_id, unsigned char *data, int datalen)
{
	struct __econet_packet_udp r;
	unsigned char username[15];
	unsigned short count;

	r.p.ptype = ECONET_AUN_DATA;
	r.p.port = reply_port;
	r.p.ctrl = 0x80;

	fs_copy_to_cr(username, (data+5), 14);

	if (!fs_quiet) fprintf (stderr, "   FS:%12sfrom %3d.%3d Read user info for %s\n", "", net, stn, username);

	count = 0;

	while (count < ECONET_MAX_FS_USERS)
	{
		if ((active[server][count].net != 0) && (active[server][count].stn != 0) && (!strncmp((const char *) username, (const char *) users[server][active[server][count].userid].username, 10)))
		{

			unsigned short userid = active[server][count].userid;
			r.p.data[0] = r.p.data[1] = 0;
			if (users[server][userid].priv & FS_PRIV_SYSTEM)
				r.p.data[2] = 1;
			else	r.p.data[2] = 0;

			r.p.data[3] = active[server][count].stn;
			r.p.data[4] = active[server][count].net;

			fs_aun_send(&r, server, 5, net, stn);
			break;
		
		}
		else count++;
	}
	
	if (count == ECONET_MAX_FS_USERS)
		fs_error(server, reply_port, net, stn, 0xBC, "No such user or not logged on");

}

// Read fileserver version number
void fs_read_version(int server, unsigned short reply_port, unsigned char net, unsigned char stn, int active_id, unsigned char *data, int datalen)
{
	struct __econet_packet_udp r;
	
	r.p.ptype = ECONET_AUN_DATA;
	r.p.port = reply_port;
	r.p.ctrl = 0x80;

	r.p.data[0] = r.p.data[1] = 0;
	sprintf((char * ) &(r.p.data[2]), "%s%d", FS_VERSION_STRING, 0x0d);

	if (!fs_quiet) fprintf (stderr, "   FS:%12sfrom %3d.%3d Read FS version\n", "", net, stn);

	fs_aun_send(&r, server, strlen(FS_VERSION_STRING)+3, net, stn);

}

// Read catalogue header
void fs_cat_header(int server, unsigned short reply_port, int active_id, unsigned char net, unsigned char stn, unsigned char *data, int datalen)
{
	unsigned char path[1024];
	unsigned short relative_to;
	struct path p;
	struct __econet_packet_udp r;
	
	relative_to = *(data+3);

	fs_copy_to_cr(path, data+5, 1022);

	if (!fs_quiet) fprintf (stderr, "   FS:%12sfrom %3d.%3d Read catalogue header %s\n", "", net, stn, path);

	if (!fs_normalize_path(server, active_id, path, relative_to, &p))
		fs_error(server, reply_port, net, stn, 0xd6, "Not found");
	else
	{
		if (p.ftype != FS_FTYPE_DIR)
			fs_error(server, reply_port, net, stn, 0xAF, "Types don't match");
		else
		{
			r.p.ptype = ECONET_AUN_DATA;
			r.p.port = reply_port;
			r.p.ctrl = 0x80;
			r.p.data[0] = r.p.data[1] = 0;

			sprintf((char * ) &(r.p.data[2]), "%-10s%c   %-15s%c%c", (char *) (p.npath == 0 ? "$" : (char *) p.path[p.npath-1]),
				(p.owner == active[server][active_id].userid ? 'O' : 'P'),
				fs_discs[server][active[server][active_id].current_disc].name,
				0x0d, 0x80);
	
			fs_aun_send(&r, server, 33, net, stn);	
		}

	}
	
}

// Load file, & cope with 'Load as command'
void fs_load(int server, unsigned short reply_port, unsigned char net, unsigned char stn, unsigned int active_id, unsigned char *data, int datalen, unsigned short loadas, unsigned char rxctrl)
{
	unsigned char command[256];
	struct path p;
	struct __econet_packet_udp r;

	FILE *f;

	unsigned char data_port = *(data+2);

	unsigned char relative_to = *(data+3);
		
	unsigned short result;

	fs_copy_to_cr(command, data+5, 256);

	if (loadas) // End the command at first space if there is one - BBC Bs seem to send the whole command line
	{
		int ptr;
		ptr = 0;
		while (ptr < strlen((const char *) command))
		{
			if (command[ptr] == ' ') command[ptr] = 0x00;
			ptr++;
		}
	}

	if (!fs_quiet) fprintf (stderr, "   FS:%12sfrom %3d.%3d %s %s\n", "", net, stn, (loadas ? "Run" : "Load"), command);

	//if (!fs_normalize_path(server, active_id, command, active[server][active_id].current, &p) &&
	if (!(result = fs_normalize_path(server, active_id, command, relative_to, &p)))
	{

		if (loadas)
			fs_error(server, reply_port, net, stn, 0xFE, "Bad command");
		else
			fs_error(server, reply_port, net, stn, 0xD6, "Not found");
		return;
	}

	if ((!result || (p.ftype == FS_FTYPE_NOTFOUND)) && loadas && !fs_normalize_path(server, active_id, command, active[server][active_id].lib, &p))   // Either in current, or lib if loadas set
	{
		if (loadas)
			fs_error(server, reply_port, net, stn, 0xFE, "Bad command");
		else
			fs_error(server, reply_port, net, stn, 0xD6, "Not found");
		return;
	}

	if (p.ftype != FS_FTYPE_FILE)
	{
		if (loadas)
			fs_error(server, reply_port, net, stn, 0xFE, "Bad command");
		else
			fs_error(server, reply_port, net, stn, 0xD6, "Not found");
		return;
	}

	// Check permissions

	if (!((active[server][active_id].priv & FS_PRIV_SYSTEM) || (p.my_perm & FS_PERM_OWN_R))) // Note: my_perm has all the relevant privilege bits in the bottom 4
	{
		fs_error(server, reply_port, net, stn, 0xBD, "Insufficient access");
		return;
	}

	// Use interlock function here
	if (!(f = fopen((const char * ) p.unixpath, "r")))
	{
		fs_error(server, reply_port, net, stn, 0xFE, "Cannot open file");
		return;
	}

	
	r.p.port = reply_port;
	r.p.ctrl = rxctrl;
	r.p.ptype = ECONET_AUN_DATA;
	r.p.data[0] = r.p.data[1] = 0;

	// Send the file attributes

	r.p.data[2] = (p.load & 0xff);
	r.p.data[3] = (p.load & 0xff00) >> 8;
	r.p.data[4] = (p.load & 0xff00) >> 16;
	r.p.data[5] = (p.load & 0xff0000) >> 24;
	r.p.data[6] = (p.exec & 0xff);
	r.p.data[7] = (p.exec & 0xff00) >> 8;
	r.p.data[8] = (p.exec & 0xff00) >> 16;
	r.p.data[9] = (p.exec & 0xff0000) >> 24;
	r.p.data[10] = p.length & 0xff;
	r.p.data[11] = (p.length & 0xff00) >> 8;
	r.p.data[12] = (p.length & 0xff0000) >> 16;
	r.p.data[13] = p.perm;
	r.p.data[14] = p.day;
	r.p.data[15] = p.monthyear;

	if (fs_aun_send(&r, server, 16, net, stn))
	{
		// Send data burst

		int collected;

		r.p.ctrl = 0x80;
		r.p.port = data_port;

		// short delay to keep certain stations happy
		usleep(180000);

		while (!feof(f))
		{
			collected = fread(&(r.p.data), 1, 1280, f);
			if (!fs_aun_send(&r, server, collected, net, stn))
				return; // We failed in some way.
			// short delay to keep certain stations happy
			//usleep(200000);

		}
		
		usleep (100000); // See if this keeps the BBC Micro happy

		// Send the tail end packet
	
		r.p.data[0] = r.p.data[1] = 0x00;
		r.p.port = reply_port;

		fs_aun_send(&r, server, 2, net, stn);

	}
	
}

// Get byte from current cursor position
void fs_getbyte(int server, unsigned char reply_port, unsigned char net, unsigned char stn, unsigned int active_id, unsigned short handle, unsigned char ctrl)
{

	if (handle < 1 || handle >= FS_MAX_OPEN_FILES || active[server][active_id].fhandles[handle].handle == -1) // Invalid handle
		fs_error(server, reply_port, net, stn, 0xDE, "Channel ?");
	else // Valid handle it appears
	{
		struct __econet_packet_udp r;
		unsigned char b; // Character read, if appropriate
		FILE *h;
		unsigned char result;
		struct stat statbuf;

		h = fs_files[server][active[server][active_id].fhandles[handle].handle].handle;

		if (fstat(fileno(h), &statbuf)) // Non-zero = error
		{
			fs_error_ctrl(server, reply_port, net, stn, ctrl, 0xFF, "FS Error on read");
			return;
		}

		if (active[server][active_id].fhandles[handle].pasteof) // Already tried to read past EOF
		{
			fs_error_ctrl(server, reply_port, net, stn, ctrl, 0xDF, "EOF");
			return;
		}

		// Put the pointer back where we were

		clearerr(h);
		fseek(h, active[server][active_id].fhandles[handle].cursor, SEEK_SET);

		b = fgetc(h);

		result = 0;

		if (ftell(h) == statbuf.st_size) result = 0x80;
		if (feof(h))
		{
			result = 0xC0; // Attempt to read past end of file
			active[server][active_id].fhandles[handle].pasteof = 1;
		}

		active[server][active_id].fhandles[handle].cursor = ftell(h);
	
		r.p.ptype = ECONET_AUN_DATA;
		r.p.port = reply_port;
		r.p.ctrl = ctrl;
		r.p.data[0] = r.p.data[1] = 0;
		r.p.data[2] = (feof(h) ? 0xfe : b);
		r.p.data[3] = result;
	
		fs_aun_send(&r, server, 4, net, stn);

	}

}

void fs_putbyte(int server, unsigned char reply_port, unsigned char net, unsigned char stn, unsigned int active_id, unsigned short handle, unsigned char ctrl, unsigned char b)
{

	if (handle < 1 || handle >= FS_MAX_OPEN_FILES || active[server][active_id].fhandles[handle].handle == -1) // Invalid handle
		fs_error_ctrl(server, reply_port, net, stn, ctrl, 0xDE, "Channel ?");
	else // Valid handle it appears
	{

		FILE *h;
		struct __econet_packet_udp r;

		if (active[server][active_id].fhandles[handle].mode < 2) // Not open for writing
		{
			fs_error_ctrl(server, reply_port, net, stn, ctrl, 0xc1, "Not open for update");
			return;
		}

		h = fs_files[server][active[server][active_id].fhandles[handle].handle].handle;

		if ((ctrl & 0x01) != active[server][active_id].fhandles[handle].sequence) // Not a duplicate
		{

			unsigned char buffer[2];

			buffer[0] = b;

			// Put the pointer back where we were

			clearerr(h);
			fseek(h, active[server][active_id].fhandles[handle].cursor, SEEK_SET);

			if (fwrite(buffer, 1, 1, h) != 1)
			{
				fs_error_ctrl(server, reply_port, net, stn, ctrl, 0xFF, "FS error writing to file");
				return;
			}

			fflush(h);

			// Update cursor
	
			active[server][active_id].fhandles[handle].cursor = ftell(h);

		}
	
		active[server][active_id].fhandles[handle].sequence = (ctrl & 0x01);

		r.p.ptype = ECONET_AUN_DATA;
		r.p.port = reply_port;
		r.p.ctrl = ctrl;
		r.p.data[0] = r.p.data[1] = 0;

		fs_aun_send(&r, server, 2, net, stn);

	}

}

// Get more than one byte from file
void fs_get_random_access_info(int server, unsigned char reply_port, unsigned char net, unsigned char stn, unsigned int active_id, unsigned short handle, unsigned short function)
{

	struct __econet_packet_udp r;

	if (active[server][active_id].fhandles[handle].handle == -1) // Invalid handle
	{
		fs_error(server, reply_port, net, stn, 0xDE, "Channel ?");
		return;
	}

	r.p.port = reply_port;
	r.p.ctrl = 0x80;
	r.p.ptype = ECONET_AUN_DATA;
	r.p.data[0] = r.p.data[1] = 0;

	switch (function) 
	{
		case 0: // Cursor position
			r.p.data[2] = (active[server][active_id].fhandles[handle].cursor & 0xff);
			r.p.data[3] = (active[server][active_id].fhandles[handle].cursor & 0xff00) >> 8;
			r.p.data[4] = (active[server][active_id].fhandles[handle].cursor & 0xff00) >> 16;
			break;
		case 1: // Fall through extent / allocation - going to assume this is file size but might be wrong
		case 2:
		{
			struct stat s;

			if (fstat(fileno(fs_files[server][active[server][active_id].fhandles[handle].handle].handle), &s)) // Non-zero == error
			{
				fs_error(server, reply_port, net, stn, 0xFF, "FS error");
				return;
			}
		
			r.p.data[2] = s.st_size & 0xff;
			r.p.data[3] = (s.st_size & 0xff00) >> 8;
			r.p.data[4] = (s.st_size & 0xff0000) >> 16;
			break;
		}
		
	}	

	fs_aun_send(&r, server, 5, net, stn);

}

// Get more than one byte from file
void fs_set_random_access_info(int server, unsigned char reply_port, unsigned char net, unsigned char stn, unsigned int active_id, unsigned short handle, unsigned char *data, unsigned short datalen)
{

	struct __econet_packet_udp r;
	unsigned short function;
	unsigned long value;
	unsigned long extent;
	FILE *f;
	struct stat s;

	if (active[server][active_id].fhandles[handle].handle == -1) // Invalid handle
	{
		fs_error(server, reply_port, net, stn, 0xDE, "Channel ?");
		return;
	}

	f = fs_files[server][active[server][active_id].fhandles[handle].handle].handle;

	if (fstat(fileno(f), &s)) // Error
	{
		fs_error(server, reply_port, net, stn, 0xFF, "FS error");
		return;
	}

	extent = s.st_size;

	if (extent < 0) // Error
	{
		fs_error(server, reply_port, net, stn, 0xFF, "FS Error");
		return;
	}

	r.p.port = reply_port;
	r.p.ctrl = 0x80;
	r.p.ptype = ECONET_AUN_DATA;
	r.p.data[0] = r.p.data[1] = 0;

	function = *(data+6);
	value = (*(data+7)) + ((*(data+8)) << 8) + ((*(data+9)) << 16);

	switch (function)
	{
		case 0: // Set pointer
		{
			if (value > extent) // Need to expand file				
			{
				unsigned char buffer[4096];
				unsigned long to_write, written;
				unsigned int chunk;

				memset (&buffer, 0, 4096);
				fseek(f, 0, SEEK_END);
		
				to_write = value - extent;
	
				while (to_write > 0)
				{

					chunk = (to_write > 4096 ? 4096 : to_write);

					written = fwrite(buffer, 1, chunk, f);
					if (written != chunk)
					{
						fprintf(stderr, "Tried to write %d, but fwrite returned %ld\n", chunk, written);
						fs_error(server, reply_port, net, stn, 0xFF, "FS Error extending file");
						return;
					}
					to_write -= written;
				}
			}

			active[server][active_id].fhandles[handle].cursor = value;
		}
		break;
		case 1: // Set file extent
		{
			if (value > extent)
                        {
                                unsigned char buffer[4096];
                                unsigned long to_write, written;

                                memset (&buffer, 0, 4096);
                                fseek(f, 0, SEEK_END);

                                to_write = value - extent;

                                while (to_write > 0)
                                {
                                        written = fwrite(buffer, (to_write > 4096 ? 4096 : to_write), 1, f);
                                        if (written != (to_write > 4096 ? 4096 : to_write))
                                        {
                                                fs_error(server, reply_port, net, stn, 0xFF, "FS Error extending file");
                                                return;
                                        }
                                        to_write -= written;
                                }
                        }

			fflush(f);

			if (value < extent)
			{
				if (ftruncate(fileno(f), value)) // Error if non-zero
				{
                                        fs_error(server, reply_port, net, stn, 0xFF, "FS Error truncating file");
                                        return;
				}
			}

		}
		break;
		default:
			fs_error(server, reply_port, net, stn, 0xFF, "FS Error - unknown function");
			return;

	}

	fs_aun_send (&r, server, 2, net, stn);
}

// Get more than one byte from file
void fs_getbytes(int server, unsigned char reply_port, unsigned char net, unsigned char stn, unsigned int active_id, unsigned short handle, unsigned char ctrl, unsigned char *data, unsigned short datalen)
{

	unsigned long bytes, offset;
	unsigned char txport, offsetstatus;
	unsigned long sent;
	unsigned short internal_handle;
	unsigned short eofreached, fserroronread;

	unsigned char readbuffer[256];

	struct __econet_packet_udp r;

	txport = *(data+2);
	offsetstatus = *(data+6);
	bytes = (((*(data+7))) + ((*(data+8)) << 8) + (*(data+9) << 16));
	offset = (((*(data+10))) + ((*(data+11)) << 8) + (*(data+12) << 16));

	if (!fs_quiet) fprintf (stderr, "   FS:%12sfrom %3d.%3d fs_getbytes() %ld from offset %ld by user %04x on handle %02x\n", "", net, stn, bytes, offset, active[server][active_id].userid, handle);

	if (active[server][active_id].fhandles[handle].handle == -1) // Invalid handle
	{
		fs_error(server, reply_port, net, stn, 0xDE, "Channel ?");
		return;
	}

	internal_handle = active[server][active_id].fhandles[handle].handle;

	if (datalen < 13)
	{
		fs_error(server, reply_port, net, stn, 0xFF, "Bad server request");
		return;
	}

	if (offsetstatus) // Read from current position
		offset = active[server][active_id].fhandles[handle].cursor;

	if (offset > ftell(fs_files[server][internal_handle].handle)) // Beyond EOF
		eofreached = 1;
	else
		eofreached = 0;

	fseek(fs_files[server][internal_handle].handle, offset, SEEK_SET);
	
	// Send acknowledge
	r.p.ptype = ECONET_AUN_DATA;
	r.p.port = reply_port;
	r.p.ctrl = ctrl;
	r.p.data[0] = r.p.data[1] = 0;

	fs_aun_send(&r, server, 2, net, stn);

	fserroronread = 0;
	sent = 0;

	while (sent < bytes && (!eofreached) && (!fserroronread))
	{
		unsigned short readlen;
		unsigned short received;

		readlen = ((bytes - sent) > sizeof(readbuffer) ? sizeof(readbuffer) : (bytes - sent));

		if ((received = fread(readbuffer, 1, readlen, fs_files[server][internal_handle].handle)) != readlen) // Either FEOF or error
		{
			if (feof(fs_files[server][internal_handle].handle)) eofreached = 1;
			else
			{
				if (!fs_quiet) fprintf(stderr, "   FS:%12sfrom %3d.%3d fread returned %d, expected %d\n", "", net, stn, received, readlen);
				fserroronread = 1;
			}
		}

		if (!fs_quiet) fprintf(stderr, "   FS:%12sfrom %3d.%3d fs_getbytes() read %04x bytes off disc\n", "", net, stn, received);

		if (received > 0) // Send what we did get
		{
			r.p.ptype = ECONET_AUN_DATA;
			r.p.port = txport;
			r.p.ctrl = 0x80;
			memcpy(&(r.p.data), readbuffer, received);	
	
			if (received < bytes) // Pad rest of data
				memset (&(r.p.data[received]), 0, bytes - received);

			// The real FS pads a short packet to the length requested, but then sends a completion message (below) indicating how many bytes were actually valid

			fs_aun_send(&r, server, bytes, net, stn);

			sent += received;
		}

			
	}
	
	usleep(100000);

	active[server][active_id].fhandles[handle].cursor = ftell(fs_files[server][internal_handle].handle);

	if (fserroronread)
		fs_error(server, reply_port, net, stn, 0xFF, "FS Error on read");
	else
	{
		// Send a completion message
	
		r.p.port = reply_port;
		r.p.ctrl = 0x80;
		r.p.data[0] = r.p.data[1] = 0;
		r.p.data[2] = (eofreached ? 0x80 : 0x00);
		r.p.data[3] = (sent & 0xff);
		r.p.data[4] = ((sent & 0xff00) >> 8);
		r.p.data[5] = ((sent & 0xff0000) >> 16);

		fs_aun_send(&r, server, 6, net, stn);
	}
	
}

void fs_putbytes(int server, unsigned char reply_port, unsigned char net, unsigned char stn, unsigned int active_id, unsigned short handle, unsigned char ctrl, unsigned char *data, unsigned short datalen)
{

	unsigned long bytes, offset;
	unsigned char txport, offsetstatus;
	unsigned short internal_handle;
	unsigned char incoming_port;

	struct __econet_packet_udp r;

	struct tm t; 
	unsigned char day, monthyear;
	time_t now;

	now = time(NULL);
	t = *localtime(&now);

	fs_date_to_two_bytes(t.tm_mday, t.tm_mon+1, t.tm_year, &monthyear, &day);
	//day = t.tm_mday;
	//monthyear = (((t.tm_year - 81 - 40) & 0x0f) << 4) | ((t.tm_mon+1) & 0x0f);	
								
	txport = *(data+2);
	offsetstatus = *(data+6);
	bytes = (((*(data+9)) << 16) + ((*(data+8)) << 8) + (*(data+7)));
	offset = (((*(data+12)) << 16) + ((*(data+11)) << 8) + (*(data+10)));

	if (active[server][active_id].fhandles[handle].handle == -1) // Invalid handle
	{
		fs_error(server, reply_port, net, stn, 0xDE, "Channel ?");
		return;
	}

	internal_handle = active[server][active_id].fhandles[handle].handle;

	if (datalen < 13)
	{
		fs_error(server, reply_port, net, stn, 0xFF, "Bad server request");
		return;
	}

	if (offsetstatus) // Read from current position
		offset = active[server][active_id].fhandles[handle].cursor;

	if (offset > ftell(fs_files[server][internal_handle].handle)) // Beyond EOF
	{
		// TODO
	}

	fseek(fs_files[server][internal_handle].handle, offset, SEEK_SET);

	// We should be the only writer, so doing the seek here should be fine
	
	// Set up a bulk transfer here.

	if ((incoming_port = fs_find_bulk_port(server)))
	{
		fs_bulk_ports[server][incoming_port].handle = internal_handle;
		fs_bulk_ports[server][incoming_port].net = net;
		fs_bulk_ports[server][incoming_port].stn = stn;
		fs_bulk_ports[server][incoming_port].ack_port = txport; // Could be wrong
		fs_bulk_ports[server][incoming_port].length = bytes;
		fs_bulk_ports[server][incoming_port].received = 0; // Initialize counter
		fs_bulk_ports[server][incoming_port].reply_port = reply_port;
		fs_bulk_ports[server][incoming_port].rx_ctrl = ctrl;
		fs_bulk_ports[server][incoming_port].mode = 3;
		fs_bulk_ports[server][incoming_port].active_id = active_id; // So that the cursor can be updated as we receive
		fs_bulk_ports[server][incoming_port].user_handle = handle;
		fs_bulk_ports[server][incoming_port].last_receive = (unsigned long long) time(NULL);
		// Send acknowledge
		r.p.ptype = ECONET_AUN_DATA;
		r.p.port = reply_port;
		r.p.ctrl = ctrl;
		r.p.data[0] = r.p.data[1] = 0;
		r.p.data[2] = incoming_port;
		r.p.data[3] = (0x500) & 0xff; // Max trf size
		r.p.data[4] = ((0x500) & 0xff00) >> 8; // High byte of max trf
	
		fs_aun_send(&r, server, 5, net, stn);
	}
	else	fs_error(server, reply_port, net, stn, 0xFF, "No channels available");

	if (bytes == 0) // No data expected
	{	
		fs_close_interlock(server, fs_bulk_ports[server][incoming_port].handle, 3);
		fs_bulk_ports[server][incoming_port].handle = -1; // Make the port available again
		r.p.port = reply_port;
		r.p.ctrl = ctrl;
		r.p.ptype = ECONET_AUN_DATA;
		r.p.data[0] = r.p.data[1] = 0;
		r.p.data[2] = FS_PERM_OWN_R | FS_PERM_OWN_W;
		r.p.data[3] = day;
		r.p.data[4] = monthyear;

		fs_aun_send (&r, server, 5, net, stn);
	}

}

void fs_eof(int server, unsigned char reply_port, unsigned char net, unsigned char stn, unsigned int active_id, unsigned short handle)
{

	unsigned char result = 0;

        if (handle < 1 || handle >= FS_MAX_OPEN_FILES || active[server][active_id].fhandles[handle].handle == -1) // Invalid handle
                fs_error(server, reply_port, net, stn, 0xDE, "Channel ?");
        else // Valid handle it appears
        {
		FILE *h;
		struct __econet_packet_udp r;

		h = fs_files[server][active[server][active_id].fhandles[handle].handle].handle;

		if (active[server][active_id].fhandles[handle].cursor == ftell(h))
			result = 1;

		r.p.ptype = ECONET_AUN_DATA;
		r.p.port = reply_port;
		r.p.ctrl = 0x80;
		r.p.data[0] = r.p.data[1] = 0;
		r.p.data[2] = result;

		fs_aun_send(&r, server, 3, net, stn);
	}

}
// Close a specific user handle. Abstracted out to allow fs_close to cycle through all handles and close them when requested close handle is 0
void fs_close_handle(int server, unsigned char reply_port, unsigned char net, unsigned char stn, unsigned int active_id, unsigned short handle)
{

	if (active[server][active_id].fhandles[handle].handle == -1) // Handle not open
		fs_error(server, reply_port, net, stn, 222, "Channel ?");
	else
	{
		if (active[server][active_id].fhandles[handle].is_dir)
			fs_deallocate_user_dir_channel (server, active_id, handle);
		else
		{
			fs_close_interlock(server, active[server][active_id].fhandles[handle].handle, active[server][active_id].fhandles[handle].mode);	
			fs_deallocate_user_file_channel(server, active_id, handle);
		}
	}
}

void fs_close(int server, unsigned char reply_port, unsigned char net, unsigned char stn, unsigned int active_id, unsigned short handle)
{

	unsigned short count;

	if (!fs_quiet) fprintf (stderr, "   FS:%12sfrom %3d.%3d Close handle %d\n", "", net, stn, handle);

	if (active[server][active_id].fhandles[handle].handle == -1) // Handle not open
	{
		fs_error(server, reply_port, net, stn, 222, "Channel ?");
		return;
	}

	count = 1;

	if (handle != 0)
		fs_close_handle(server, reply_port, net, stn, active_id, handle);
	else // User wants to close everything
	{
		while (count < FS_MAX_OPEN_FILES)
		{	
			if (active[server][active_id].fhandles[count].handle != -1)
				fs_close_handle(server, reply_port, net, stn, active_id, count);
			count++;
		}
	}

	fs_reply_success(server, reply_port, net, stn, 0, 0);

}

// Open a file, with interlock
void fs_open(int server, unsigned char reply_port, unsigned char net, unsigned char stn, unsigned int active_id, unsigned char * data, unsigned short datalen)
{

	unsigned char existingfile = *(data+5);
	unsigned char readonly = *(data+6);
	unsigned char filename[1024];
	unsigned short result;
	unsigned short count, start;
	short handle;
	struct path p;
	struct __econet_packet_udp reply;

	count = 7;
	while (*(data+count) == ' ' && count < datalen)
		count++;

	if (count == datalen)
		fs_error(server, reply_port, net, stn, 0xD6, "Not found");

	start = count;

	while (*(data+count) != ' ' && count < datalen)
		count++;

	if (count != datalen) // space in the filename!
		*(data+count) = 0x0d; // So terminate it early

	fs_copy_to_cr(filename, data+start, 1023);

	if (!fs_quiet) fprintf (stderr, "   FS:%12sfrom %3d.%3d Open %s readonly %s, must exist? %s\n", "", net, stn, filename, (readonly ? "yes" : "no"), (existingfile ? "yes" : "no"));

	result = fs_normalize_path(server, active_id, filename, active[server][active_id].current, &p);

	if (!result)
		fs_error(server, reply_port, net, stn, 0xD6, "Not found");
	else if (existingfile && p.ftype == FS_FTYPE_NOTFOUND)
	{
		fs_error(server, reply_port, net, stn, 0xD6, "Not found");

		//reply.p.ptype = ECONET_AUN_DATA;
		//reply.p.port = reply_port;
		//reply.p.ctrl = 0x80;
		//reply.p.data[0] = reply.p.data[1] = reply.p.data[2] = 0;
	
		//fs_aun_send(&reply, server, 3, net, stn);
	}
	else if ((p.ftype == FS_FTYPE_FILE) && !readonly && ((p.my_perm & FS_PERM_OWN_W) == 0))
		fs_error(server, reply_port, net, stn, 0xbd, "Insufficient access");
	//else if (existingfile && p.ftype != FS_FTYPE_FILE) // Cope with weird FS3 behaviour where you can open a directory but not actually read or write from/to it
		//fs_error(server, reply_port, net, stn, 0xBE, "Is not a file");
	else if (!readonly && (p.perm & FS_PERM_L)) // File locked
		fs_error(server, reply_port, net, stn, 0xC3, "Locked");
	else if (!readonly && (p.ftype == FS_FTYPE_NOTFOUND) && 
		(	(p.parent_owner != active[server][active_id].userid && ((p.parent_perm & FS_PERM_OTH_W) == 0)) ||
			(p.parent_owner == active[server][active_id].userid && ((p.perm & FS_PERM_OWN_W) == 0))
			) // FNF and we can't write to the directory
		)
		fs_error(server, reply_port, net, stn, 0xbd, "Insufficient access");
	else
	{

		unsigned short userhandle, mode;

		// Do we have capacity to open this file?

		mode = (readonly ? 1 : existingfile ? 2 : 3);		

		userhandle = fs_allocate_user_file_channel(server, active_id);
	
		if (userhandle)
		{
			handle = fs_open_interlock(server, p.unixpath, (readonly ? 1 : existingfile ? 2 : 3), active[server][active_id].userid);
			
			if (handle == -1)  // Couldn't open a file when we think we should be able to
			{
				fs_error(server, reply_port, net, stn, 0xFF, "FS Error");
				fs_deallocate_user_file_channel(server, active_id, userhandle);
			}
			else if (handle == -2) // Interlock issue
			{
				fs_error(server, reply_port, net, stn, 0xC2, "Already open");
				fs_deallocate_user_file_channel(server, active_id, userhandle);
			}
			else if (handle == -3)
			{
				fs_error(server, reply_port, net, stn, 0xC0, "Too many open files");
				fs_deallocate_user_file_channel(server, active_id, userhandle);
			}
			else
			{	
				active[server][active_id].fhandles[userhandle].handle = handle;
				active[server][active_id].fhandles[userhandle].mode = mode;
				active[server][active_id].fhandles[userhandle].cursor = 0;	
				active[server][active_id].fhandles[userhandle].sequence = 2; // Initialize - should only be 0, or 1 so 2 means first input always treated as non-duplicate
				active[server][active_id].fhandles[userhandle].pasteof = 0; // Not past EOF yet
				strcpy(active[server][active_id].fhandles[userhandle].acornfullpath, p.acornfullpath);
				reply.p.ptype = ECONET_AUN_DATA;
				reply.p.port = reply_port;
				reply.p.ctrl = 0x80;
				reply.p.data[0] = reply.p.data[1] = 0;
				reply.p.data[2] = (unsigned char) (userhandle & 0xff);
	
				if (!fs_quiet) fprintf (stderr, "   FS:%12sfrom %3d.%3d Opened handle %d\n", "", net, stn, userhandle);
				fs_aun_send(&reply, server, 3, net, stn);
			}
		}
		else
			fs_error(server, reply_port, net, stn, 0xC0, "Too many open files");
	}

}

// Check if a user exists. Return index into users[server] if it does; -1 if not
int fs_user_exists(int server, unsigned char *username)
{
	int count;
	unsigned short found = 0;
	char username_padded[11];

	snprintf(username_padded, 11, "%-10s", username);

	count = 0;

	while (!found && count < ECONET_MAX_FS_USERS)
	{
		if (!strncasecmp((const char *) users[server][count].username, username_padded, 10) && (users[server][count].priv != FS_PRIV_INVALID))
			found = 1;
		else count++;
	}

	if (count == ECONET_MAX_FS_USERS) return -1;
	else return count;
	 
}

// Returns -1 if there are no user slots available, or the slot number if there are
short fs_find_new_user(int server)
{

	int count = 0;
	unsigned short found = 0;

	while (!found && count < ECONET_MAX_FS_USERS)
	{
		if (users[server][count].priv == FS_PRIV_INVALID)
			found = 1;
		else count++;
	}

	if (count == ECONET_MAX_FS_USERS) return -1;
	else return count;

}

// Handle incoming file / data transfers
void handle_fs_bulk_traffic(int server, unsigned char net, unsigned char stn, unsigned char port, unsigned char ctrl, unsigned char *data, unsigned int datalen)
{

	struct __econet_packet_udp r;

	// Do you know this man?

	if (		(fs_bulk_ports[server][port].handle != -1) && 
		 	(fs_bulk_ports[server][port].net == net) &&
			(fs_bulk_ports[server][port].stn == stn) 
	)
	{
		// We can deal with this data
	
		fwrite(data, 1, datalen, fs_files[server][fs_bulk_ports[server][port].handle].handle);
	
		fs_bulk_ports[server][port].received += datalen;

		if (fs_bulk_ports[server][port].user_handle != 0) // This is a putbytes transfer not a fs_save; in the latter there is no user handle
			active[server][fs_bulk_ports[server][port].active_id].fhandles[fs_bulk_ports[server][port].user_handle].cursor += datalen;
	
		fs_bulk_ports[server][port].last_receive = (unsigned long long) time(NULL);

		if (fs_bulk_ports[server][port].received == fs_bulk_ports[server][port].length) // Finished
		{

			// Send a closing ACK

			struct tm t; 
			unsigned char day, monthyear;
			time_t now;

			now = time(NULL);
			t = *localtime(&now);

			fs_date_to_two_bytes(t.tm_mday, t.tm_mon+1, t.tm_year, &monthyear, &day);
			//day = t.tm_mday;
			//monthyear = (((t.tm_year - 81 - 40) & 0x0f) << 4) | ((t.tm_mon+1) & 0x0f);	
								
			r.p.port = fs_bulk_ports[server][port].reply_port;
			r.p.ctrl = fs_bulk_ports[server][port].rx_ctrl;
			r.p.ptype = ECONET_AUN_DATA;
			r.p.data[0] = r.p.data[1] = 0;

			if (fs_bulk_ports[server][port].user_handle) // This was PutBytes, not save
			{
				r.p.data[2] = port;
				r.p.data[3] = fs_bulk_ports[server][port].received & 0xff;
				r.p.data[4] = (fs_bulk_ports[server][port].received & 0xff00) >> 8;
				r.p.data[5] = (fs_bulk_ports[server][port].received & 0xff0000) >> 16;
				fs_aun_send (&r, server, 6, net, stn);
			}
			else // Was a save
			{
				
				fs_close_interlock(server, fs_bulk_ports[server][port].handle, 3); // We don't close on a putbytes - file stays open!

				r.p.data[2] = FS_PERM_OWN_R | FS_PERM_OWN_W;
				r.p.data[3] = day;
				r.p.data[4] = monthyear;

				fs_aun_send (&r, server, 5, net, stn);
			}

			fs_bulk_ports[server][port].handle = -1; // Make the bulk port available again

		}
		else
		{	
			r.p.port = fs_bulk_ports[server][port].ack_port;
			r.p.ctrl = ctrl;
			r.p.ptype = ECONET_AUN_DATA;
			r.p.data[0] = 0; // was FS_PERM_OWN_R | FS_PERM_OWN_W;
			fs_aun_send (&r, server, 1, net, stn);
		}

		

	}
	// Otherwise, er.... ignore it?
	
}

/* Garbage collect stale incoming bulk handles - This is called from the main loop in the bridge code */

void fs_garbage_collect(int server)
{

	int count; // == Bulk port number

	for (count = 1; count < 255; count++) // Start at 1 because port 0 is immediates...
	{
		if (fs_bulk_ports[server][count].handle != -1) // Operating handle
		{
			if (fs_bulk_ports[server][count].last_receive < ((unsigned long long) time(NULL) - 10)) // 10 seconds and no traffic
			{
				if (!fs_quiet) fprintf (stderr, "   FS:%12sfrom %3d.%3d Garbage collecting stale incoming bulk port %d used %lld seconds ago\n", "", 
					fs_bulk_ports[server][count].net, fs_bulk_ports[server][count].stn, count, ((unsigned long long) time(NULL) - fs_bulk_ports[server][count].last_receive));

				fs_close_interlock(server, fs_bulk_ports[server][count].handle, fs_bulk_ports[server][count].mode);

				if (fs_bulk_ports[server][count].active_id != 0) // No user handle - this was a SAVE operation
					fs_deallocate_user_file_channel(server, fs_bulk_ports[server][count].active_id, fs_bulk_ports[server][count].user_handle);
			}

		}

	}

}

/* Handle locally arriving fileserver traffic to server #server, from net.stn, ctrl, data, etc. - port will be &99 for FS Op */
void handle_fs_traffic (int server, unsigned char net, unsigned char stn, unsigned char ctrl, unsigned char *data, unsigned int datalen)
{

	unsigned char fsop, reply_port; // , root_dir, current_dir, lib_dir;
	unsigned int userid;
	unsigned int active_id;

	if (datalen < 1) 
	{
		if (!fs_quiet) fprintf (stderr, "   FS: from %3d.%3d Invalid FS Request with no data\n", net, stn);
		return;
	}


	reply_port = *data;
	fsop = *(data+1);
	//root_dir = *(data+2);
	//current_dir = *(data+3);
	//lib_dir = *(data+4);

	active_id = fs_stn_logged_in(server, net, stn);

	if ((active_id < 0) && (fsop != 0)) // Not logged in and not OSCLI (so can't be *I AM)
	{
		fs_error(server, reply_port, net, stn, 0xbf, "Who are you?");
		return;
	}

	userid = fs_find_userid(server, net, stn);

	if (userid < 0)
		fs_error(server, reply_port, net, stn, 0xBC, "User not known");

	switch (fsop)
	{
		case 0: // OSCLI
		{
			unsigned char command[256];
			int counter;

			counter = 5;
			while ((*(data+counter) != 0x0d) && (counter < datalen))
			{
				command[counter-5] = *(data+counter);
				counter++;
			}
			command[counter-5] = 0;

			if (!strncasecmp("I AM ", (const char *) command, 5)) fs_login(server, reply_port, net, stn, command + 5);
			else if (!strncasecmp("LOGIN ", (const char *) command, 6)) fs_login(server, reply_port, net, stn, command + 6);
			else if (!strncasecmp("IAM ", (const char *) command, 4)) fs_login(server, reply_port, net, stn, command + 4);
			else if (fs_stn_logged_in(server, net, stn) < 0)
				fs_error(server, reply_port, net, stn, 0xbf, "Who are you ?");
			else if (!strncasecmp("BYE", (const char *) command, 3)) fs_bye(server, reply_port, net, stn);
			else if (!strncasecmp("SETLIB ", (const char *) command, 7))
			{ // Permanently set library directory
				unsigned char libdir[96];

				if (active[server][active_id].priv & FS_PRIV_LOCKED)
					fs_error(server, reply_port, net, stn, 0xbd, "Insufficient access");
				else
				{
					struct path p;
					fs_copy_to_cr(libdir, command+7, 93);
					if (fs_normalize_path(server, active_id, libdir, *(data+3), &p) && (p.ftype == FS_FTYPE_DIR) && strlen((const char *) p.path_from_root) < 94 && (p.disc == users[server][userid].home_disc))
					{
						if (strlen(p.path_from_root) > 0)
						{
							users[server][userid].lib[0] = '$';
							users[server][userid].lib[1] = '.';
							users[server][userid].lib[2] = '\0';
						}
						else	strcpy(users[server][userid].lib, "");

						strncat((char * ) users[server][userid].lib, (const char * ) p.path_from_root, 94);
						fs_write_user(server, userid, (unsigned char *) &(users[server][userid]));
						fs_reply_ok(server, reply_port, net, stn);
					}
					else	fs_error(server, reply_port, net, stn, 0xA8, "Bad library");
				}
			}
			else if (!strncasecmp("PASS ", (const char *) command, 5))
				fs_change_pw(server, reply_port, userid, net, stn, command+5);
			else if (!strncasecmp("CHOWN ", (const char *) command, 6))
				fs_chown(server, reply_port, active_id, net, stn, command+6);
			else if (!strncasecmp("OWNER ", (const char *) command, 6))
				fs_owner(server, reply_port, active_id, net, stn, command+6);
			else if (!strncasecmp("ACCESS ", (const char *) command, 7))
				fs_access(server, reply_port, active_id, net, stn, command+7);
			else if (!strncasecmp("INFO ", (const char *) command, 5))
				fs_info(server, reply_port, active_id, net, stn, command+5);
			else if (!strncasecmp("I.", (const char *) command, 2))
				fs_info(server, reply_port, active_id, net, stn, command+2);
			else if (!strncasecmp("CDIR ", (const char *) command, 5))
				fs_cdir(server, reply_port, active_id, net, stn, active[server][active_id].current, command+5);
			else if (!strncasecmp("DELETE ", (const char *) command, 7))
				fs_delete(server, reply_port, active_id, net, stn, active[server][active_id].current, command+7);
			else if (!strncasecmp("RENAME ", (const char *) command, 7))
				fs_rename(server, reply_port, active_id, net, stn, active[server][active_id].current, command+7);
			else if (!strncasecmp("REN. ", (const char *) command, 5))
				fs_rename(server, reply_port, active_id, net, stn, active[server][active_id].current, command+5);
			else if (!strncasecmp("SDISC ", (const char *) command, 6))
				fs_sdisc(server, reply_port, active_id, net, stn, command + 6);
			else if (!strncasecmp("COPY ", (const char *) command, 5))
				fs_copy(server, reply_port, active_id, net, stn, command+5);
			else if (!strncasecmp("LIB ", (const char *) command, 4)) // Change library directory
			{
				int found;
				struct path p;
				unsigned short l, n_handle;

				if (!fs_quiet) fprintf (stderr, "   FS:%12sfrom %3d.%3d LIB %s\n", "", net, stn, command+4);
				if ((found = fs_normalize_path(server, active_id, command+4, *(data+3), &p)) && (p.ftype != FS_FTYPE_NOTFOUND)) // Successful path traverse
				{
					if (p.ftype != FS_FTYPE_DIR)
						fs_error(server, reply_port, net, stn, 0xAF, "Types don't match");
					else
					{	
						l = fs_get_dir_handle(server, active_id, p.unixpath);
						if (l != -1) // Found
						{
							n_handle = fs_allocate_user_dir_channel(server, active_id, l);
							if (n_handle >= 0)
							{
								int old;
								struct __econet_packet_udp r;

								old = active[server][active_id].lib;

								active[server][active_id].lib = n_handle;
								strncpy((char * ) active[server][active_id].lib_dir, (const char * ) p.path_from_root, 255);
								if (p.npath == 0)	strcpy((char * ) active[server][active_id].lib_dir_tail, (const char * ) "$         ");
								else			sprintf(active[server][active_id].lib_dir_tail, "%-10s", p.path[p.npath-1]);
								
								active[server][active_id].lib_disc = p.disc;
								strcpy(active[server][active_id].fhandles[n_handle].acornfullpath, p.acornfullpath);

								if (old > 0) fs_deallocate_user_dir_channel(server, active_id, old);

								r.p.ptype = ECONET_AUN_DATA;
								r.p.port = reply_port;
								r.p.ctrl = 0x80;
								r.p.data[0] = 0x09; // Changed directory;
								r.p.data[1] = 0x00;
								r.p.data[2] = n_handle;
								fs_aun_send (&r, server, 3, net, stn);
							
							}
							else	fs_error(server, reply_port, net, stn, 0xC0, "Too many open directories");
						}
						else	fs_error(server, reply_port, net, stn, 0xD6, "Dir unreadable");
					}
				}
				else	fs_error(server, reply_port, net, stn, 0xFE, "Not found");
			}
			else if (!strncasecmp("DIR ", (const char *) command, 4)) // Change library directory
			{
				int found;
				struct path p;
				unsigned short l, n_handle;

				if (!fs_quiet) fprintf (stderr, "   FS:%12sfrom %3d.%3d DIR %s\n", "", net, stn, command+4);
				if ((found = fs_normalize_path(server, active_id, command+4, *(data+3), &p)) && (p.ftype != FS_FTYPE_NOTFOUND)) // Successful path traverse
				{
					if (p.ftype != FS_FTYPE_DIR)
						fs_error(server, reply_port, net, stn, 0xAF, "Types don't match");
					else
					{	
						l = fs_get_dir_handle(server, active_id, p.unixpath);
						if (l != -1) // Found
						{
							n_handle = fs_allocate_user_dir_channel(server, active_id, l);
							if (n_handle >= 0)
							{
								int old;
								struct __econet_packet_udp r;
								
								old = active[server][active_id].current;
								active[server][active_id].current = n_handle;
								strncpy((char * ) active[server][active_id].current_dir, (const char * ) p.path_from_root, 255);
								if (p.npath == 0)	strcpy((char * ) active[server][active_id].current_dir_tail, (const char * ) "$         ");
								else			sprintf(active[server][active_id].current_dir_tail, "%-10s", p.path[p.npath-1]);
								
								active[server][active_id].current_disc = p.disc;
								strcpy(active[server][active_id].fhandles[n_handle].acornfullpath, p.acornfullpath);
								if (old > 0) fs_deallocate_user_dir_channel(server, active_id, old);
								r.p.ptype = ECONET_AUN_DATA;
								r.p.port = reply_port;
								r.p.ctrl = 0x80;
								r.p.data[0] = 0x07; // Changed directory;
								r.p.data[1] = 0x00;
								r.p.data[2] = n_handle;
								fs_aun_send (&r, server, 3, net, stn);
	
							
							}
							else	fs_error(server, reply_port, net, stn, 0xC0, "Too many open directories");
						}
						else	fs_error(server, reply_port, net, stn, 0xC7, "Dir unreadable");
					}
				}
				else	fs_error(server, reply_port, net, stn, 0xFE, "Not found");
			}

			else if (active[server][active_id].priv & FS_PRIV_SYSTEM)
			{

				// System commands here

				if (!strncasecmp("SETHOME ", (const char *) command, 8))
				{ // Permanently set home directory
					unsigned char dir[96];
	
					{
						struct path p;
						fs_copy_to_cr(dir, command+8, 93);
						if (fs_normalize_path(server, active_id, dir, *(data+3), &p) && (p.ftype == FS_FTYPE_DIR) && strlen((const char *) p.path_from_root) < 94)
						{
							if (strlen(p.path_from_root) > 0)
							{
								users[server][userid].home[0] = '$';
								users[server][userid].home[1] = '.';
								users[server][userid].home[2] = '\0';
							}
							else	strcpy(users[server][userid].home, "");
	
							strncat((char * ) users[server][userid].home, (const char * ) p.path_from_root, 94);
							users[server][userid].home_disc = p.disc;
							fs_write_user(server, userid, (unsigned char *) &(users[server][userid]));
							fs_reply_ok(server, reply_port, net, stn);
						}
						else	fs_error(server, reply_port, net, stn, 0xA8, "Bad directory");
					}
				}
				else if (!strncasecmp("LINK ", (const char *) command, 5))
					fs_link(server, reply_port, active_id, net, stn, command+5);
				else if (!strncasecmp("FLOG ", (const char *) command, 5)) // Force log user off
				{
					char parameter[20];
					unsigned short l_net, l_stn;

					fs_copy_to_cr(parameter, command+5, 19);

					if (isdigit(parameter[0])) // Assume station number, possible net number too
					{
						if (sscanf(parameter, "%hd.%hd", &l_net, &l_stn) != 2)
						{
							if (sscanf(parameter, "%hd", &l_stn) != 1)
								fs_error(server, reply_port, net, stn, 0xFF, "Bad station specification");
							else	l_net = 0;
						}

						if (!fs_quiet) fprintf(stderr, "   FS:%12sfrom %3d.%3d Force log off station %d.%d\n", "", net, stn, l_net, l_stn);

					}
					else // Username
					{
						if (!fs_quiet) fprintf(stderr, "   FS%12sfrom %3d.%3d Force log off user %s\n", "", net, stn, parameter);

					}
			
					fs_reply_ok(server, reply_port, net, stn);
				}
				else if (!strncasecmp("NEWUSER ", (const char *) command, 8)) // Create new user
				{
					unsigned char username[11];
					int ptr;
	
					fs_copy_to_cr(username, command+8, 10);
					
					if (!fs_quiet) fprintf (stderr, "   FS:%12sfrom %3d.%3d Create new user %s\n", "", net, stn, username);
	
					ptr = 0;
					while (ptr < 10 && username[ptr] != ' ')
						ptr++;
	
					if (ptr > 10)
					{
						fs_error(server,reply_port, net, stn, 0xD6, "Bad command");
						return;
					}
			
					username[ptr] = '\0';

					ptr++; // Now points to full name

					if (fs_user_exists(server, username) >= 0)
						fs_error(server, reply_port, net, stn, 0xFF, "User exists");
					else
					{
						int id;
						unsigned char homepath[2048];
						unsigned char idtext[5];

						id = fs_find_new_user(server);
		
						if (id < 0)
							fs_error(server, reply_port, net, stn, 0xFF, "No available users");
						else
						{
							snprintf((char * ) users[server][id].username, 11, "%-10s", username);
							snprintf((char * ) users[server][id].password, 7, "%-6s", "");
							snprintf((char * ) users[server][id].fullname, 31, "%-30s", &(username[ptr]));
							snprintf((char * ) users[server][id].home, 97, "$.%s", username);
							snprintf((char * ) users[server][id].lib, 97, "$.%s", "Library");
							users[server][id].home_disc = 0;
							users[server][id].priv = FS_PRIV_USER;
							sprintf((char * ) homepath, "%s/%1x%s/%s", fs_stations[server].directory, 0, fs_discs[server][0].name, username);
							if (mkdir((const char *) homepath, 0644) != 0)
								fs_error(server, reply_port, net, stn, 0xff, "Unable to create home directory");
							else
							{
								snprintf((char * ) idtext, 5, "%04x", (unsigned short) id);
								fs_write_xattr(homepath, id, FS_PERM_OWN_W | FS_PERM_OWN_R, 0, 0);
								fs_write_user(server, id, (unsigned char *) &(users[server][id]));
								if (id >= fs_stations[server].total_users) fs_stations[server].total_users = id+1;
								fs_reply_ok(server, reply_port, net, stn);
								if (!fs_quiet) fprintf (stderr, "   FS:%12sfrom %3d.%3d New User %s, id = %d, total users = %d\n", "", net, stn, username, id, fs_stations[server].total_users);
							}
							
						}
					}

				}
				else if (!strncasecmp("PRIV ", (const char *) command, 5)) // Set user privilege
				{
					char username[11], priv, priv_byte;

					unsigned short count;
		
					count = 0;
				
					while ((count +5) < strlen((const char *) command) && (count < 10) && command[count+5] != ' ')
					{
						username[count] = command[count+5];
						count++;
					}

					if ((count + 5) == strlen((const char *) command)) // THere was no space after the username
						fs_error(server, reply_port, net, stn, 0xFE, "Bad command");
					else
					{
						username[count] = '\0';
						count++;
						if ((count + 5) == strlen((const char *) command)) // There was no priv character!
							fs_error(server, reply_port, net, stn, 0xFE, "Bad command");
						else
						{
							priv = command[count+5];	
							switch (priv) {
								case 'S': // System user
									priv_byte = FS_PRIV_SYSTEM;			
									break;
								case 'U': // Unlocked normal user
									priv_byte = FS_PRIV_USER;
									break;
								case 'L': // Locked normal user
									priv_byte = FS_PRIV_LOCKED;
									break;
								case 'N': // Unlocked user who cannot change password
									priv_byte = FS_PRIV_NOPASSWORDCHANGE;
									break;
								case 'D': // Invalidate privilege - delete the user
									priv_byte = 0;
									break;
								default:
									priv_byte = 0xff;
									fs_error(server, reply_port, net, stn, 0xfe, "Bad command");
									break;
							}

							if (priv_byte != 0xff) // Valid change
							{
								unsigned short found = 0;
								count = 0;
								char username_padded[11];
			
								// Find user
		
								snprintf(username_padded, 11, "%-10s", username);
								
								while ((count < ECONET_MAX_FS_USERS) && !found)
								{
									if (!strncasecmp((const char *) users[server][count].username, username_padded, 10) && users[server][count].priv != FS_PRIV_INVALID)
									{
										if (!fs_quiet) fprintf (stderr, "   FS:%12sfrom %3d.%3d Change privilege for %s to %02x\n", "", net, stn, username, priv_byte);

										users[server][count].priv = priv_byte;
										fs_write_user(server, count, (unsigned char *) &(users[server][count]));
										fs_reply_ok(server, reply_port, net, stn);
										found = 1;
									}
									count++;
								}
								
								if (count == ECONET_MAX_FS_USERS) fs_error(server, reply_port, net, stn, 0xbc, "User not found");

							}
						}
					}
				}
				else // Unknown command
				{

					struct __econet_packet_udp r;
					unsigned short counter;
		
					r.p.port = reply_port;
					r.p.ctrl = 0x80;
					r.p.ptype = ECONET_AUN_DATA;
					r.p.data[0] = 0x08; // Unknown command
					r.p.data[1] = 0x00; // Unknown command
					counter = 0;
					while (counter < (datalen-5))
					{
						r.p.data[2+counter] = data[counter+5];
						counter++;
					}
	
					fs_aun_send(&r, server, 2+counter, net, stn);

				}
			}

			// Unknown command. 

			else 
			{
				struct __econet_packet_udp r;
				unsigned short counter;
	
				r.p.port = reply_port;
				r.p.ctrl = 0x80;
				r.p.ptype = ECONET_AUN_DATA;
				r.p.data[0] = 0x08; // Unknown command
				r.p.data[1] = 0x00; // Unknown command
				counter = 0;
				while (counter < (datalen-5))
				{
					r.p.data[2+counter] = data[counter+5];
					counter++;
				}

				fs_aun_send(&r, server, 2+counter, net, stn);

			}
				
			
		}
		break;
		case 0x01: if (fs_stn_logged_in(server, net, stn) >= 0) fs_save(server, reply_port, net, stn, active_id, data, datalen, ctrl); else fs_error(server, reply_port, net, stn, 0xbf, "Who are you ?"); // Save file
			break;
		case 0x02: if (fs_stn_logged_in(server, net, stn) >= 0) fs_load(server, reply_port, net, stn, active_id, data, datalen, 0, ctrl); else fs_error(server, reply_port, net, stn, 0xbf, "Who are you ?"); // Load without searching library
			break;
		case 0x03: // Examine directory
			if (fs_stn_logged_in(server, net, stn) >= 0) fs_examine(server, reply_port, net, stn, active_id, data, datalen); else fs_error(server, reply_port, net, stn, 0xbf, "Who are you ?");
			break;
		case 0x04: // Catalogue header
			if (fs_stn_logged_in(server, net, stn) >= 0) fs_cat_header(server, reply_port, net, stn, active_id, data, datalen); else fs_error(server, reply_port, net, stn, 0xbf, "Who are you ?");
			break;
		case 0x05: if (fs_stn_logged_in(server, net, stn) >= 0) fs_load(server, reply_port, net, stn, active_id, data, datalen, 1, ctrl); else fs_error(server, reply_port, net, stn, 0xbf, "Who are you ?"); // Load with library search
			break;
		case 0x06: // Open file
			if (fs_stn_logged_in(server, net, stn) >= 0) fs_open(server, reply_port, net, stn, active_id, data, datalen); else fs_error(server, reply_port, net, stn, 0xbf, "Who are you ?");
			break;
		case 0x07: // Close file
			if (fs_stn_logged_in(server, net, stn) >= 0) fs_close(server, reply_port, net, stn, active_id, *(data+5)); else fs_error(server, reply_port, net, stn, 0xbf, "Who are you ?");
			// Experimentation with *Findlib reveals that the handle sought to be closed is actually at data+4 not data+5. Not sure what data+5 is then. Except that sometimes it IS in byte data+5. Maybe if it's a directory, it's in data+4 and a file is in data+5...
			//if (fs_stn_logged_in(server, net, stn) >= 0) fs_close(server, reply_port, net, stn, active_id, *(data+4)); else fs_error(server, reply_port, net, stn, 0xbf, "Who are you ?");
			break;
		case 0x08: // Get byte
			if (fs_stn_logged_in(server, net, stn) >= 0) fs_getbyte(server, reply_port, net, stn, active_id, *(data+2), ctrl); else fs_error(server, reply_port, net, stn, 0xbf, "Who are you ?");
			break;
		case 0x09: // Put byte
			if (fs_stn_logged_in(server, net, stn) >= 0) fs_putbyte(server, reply_port, net, stn, active_id, *(data+2), ctrl, *(data+3)); else fs_error(server, reply_port, net, stn, 0xbf, "Who are you ?");
			break;
		case 0x0a: // Get bytes
			if (fs_stn_logged_in(server, net, stn) >= 0) fs_getbytes(server, reply_port, net, stn, active_id, *(data+5), ctrl, data, datalen); else fs_error(server, reply_port, net, stn, 0xbf, "Who are you ?");
			break;
		case 0x0b: // Put bytes
			if (fs_stn_logged_in(server, net, stn) >= 0) fs_putbytes(server, reply_port, net, stn, active_id, *(data+5), ctrl, data, datalen); else fs_error(server, reply_port, net, stn, 0xbf, "Who are you ?");
			break;
		case 0x0c: // Get Random Access Info
			fs_get_random_access_info(server, reply_port, net, stn, active_id, *(data+5), *(data+6));
			break;
		case 0x0d: // Set Random Access Info
			fs_set_random_access_info(server, reply_port, net, stn, active_id, *(data+5), data, datalen);
			break;
		case 0x0e: // Read disc names
			if (fs_stn_logged_in(server, net, stn) >= 0) fs_read_discs(server, reply_port, net, stn, active_id, data, datalen); else fs_error(server, reply_port, net, stn, 0xbf, "Who are you ?");
			break;
		case 0x0f: // Read logged on users
			if (fs_stn_logged_in(server, net, stn) >= 0) fs_read_logged_on_users(server, reply_port, net, stn, active_id, data, datalen); else fs_error(server, reply_port, net, stn, 0xbf, "Who are you ?");
			break;
		case 0x10: // Read time
			if (fs_stn_logged_in(server, net, stn) >= 0) fs_read_time(server, reply_port, net, stn, active_id, data, datalen); else fs_error(server, reply_port, net, stn, 0xbf, "Who are you ?");
			break;
		case 0x11: // Read end of file status 
			if (fs_stn_logged_in(server, net, stn) >= 0) fs_eof(server, reply_port, net, stn, active_id, *(data+2)); else fs_error(server, reply_port, net, stn, 0xbf, "Who are you ?");
			break;
		case 0x12: // Read object info
			if (fs_stn_logged_in(server, net, stn) >= 0) fs_get_object_info(server, reply_port, net, stn, active_id, data, datalen); else fs_error(server, reply_port, net, stn, 0xbf, "Who are you ?");
			break;
		case 0x13: // Set object info
			if (fs_stn_logged_in(server, net, stn) >= 0) fs_set_object_info(server, reply_port, net, stn, active_id, data, datalen); else fs_error(server, reply_port, net, stn, 0xbf, "Who are you ?");
			break;
		case 0x14: // Delete object
			if (fs_stn_logged_in(server, net, stn) >= 0) fs_delete(server, reply_port, active_id, net, stn, active[server][active_id].current, (data+5)); else fs_error(server, reply_port, net, stn, 0xbf, "Who are you ?");
			break;
		case 0x15: // Read user environment
			if (fs_stn_logged_in(server, net, stn) >= 0) fs_read_user_env(server, reply_port, net, stn, active_id); else fs_error(server, reply_port, net, stn, 0xbf, "Who are you ?");
			break;
		case 0x16: // Set boot opts
			if (fs_stn_logged_in(server, net, stn) >= 0) fs_set_bootopt(server, reply_port, userid, net, stn, data); else fs_error(server, reply_port, net, stn, 0xbf, "Who are you ?");
			break;
		case 0x17: // BYE
			if (fs_stn_logged_in(server, net, stn) >= 0) fs_bye(server, reply_port, net, stn); else fs_error(server, reply_port, net, stn, 0xbf, "Who are you ?");
			break;
		case 0x18: // Read user info
			if (fs_stn_logged_in(server, net, stn) >= 0) fs_read_user_info(server, reply_port, net, stn, active_id, data, datalen); else fs_error(server, reply_port, net, stn, 0xbf, "Who are you ?");
			break;
		case 0x19: // Read FS version
			if (fs_stn_logged_in(server, net, stn) >= 0) fs_read_version(server, reply_port, net, stn, active_id, data, datalen); else fs_error(server, reply_port, net, stn, 0xbf, "Who are you ?");
			break;
		case 0x1a: // Read free space
			if (fs_stn_logged_in(server, net, stn) >= 0) fs_free(server, reply_port, net, stn, active_id, data, datalen); else fs_error(server, reply_port, net, stn, 0xbf, "Who are you ?");
			break;
		case 0x1b: // Create directory ??
			if (fs_stn_logged_in(server, net, stn) >= 0) fs_cdir(server, reply_port, active_id, net, stn, *(data+5), (data+6)); else fs_error(server, reply_port, net, stn, 0xbf, "Who are you ?");
			break;
		// According to the excellent Arduino Filestore code, 28 is set FS clock, 29 is create file, 30 read user free space, 31 set user free space, 32 read client id, 33 read current users extended, 34 read user information extended,
		// 35 reserved, 36 "manager interface", 37 reserved.
		case 0x1e: // Read user free space - but we aren't implementing quotas at the moment
			if ((fs_stn_logged_in(server, net, stn) >= 0) && (active[server][active_id].priv & FS_PRIV_SYSTEM))
			{
				//fs_get_user_free_space(server, reply_port, active_id, net, stn, data); // data+5 has 0x0d terminated username
				fs_reply_success(server, reply_port, net, stn, 0, 0);
			}
			break;
		case 0x1f: // Set user free space
			if ((fs_stn_logged_in(server, net, stn) >= 0) && (active[server][active_id].priv & FS_PRIV_SYSTEM))
			{
				//fs_set_user_free_space(server, reply_port, active_id, net, stn, data); // data+5 has little endian free space; data+9 has 0x0d terminated username
				fs_reply_success(server, reply_port, net, stn, 0, 0);
			}
			break;
		default: // Send error
		{
			if (!fs_quiet) fprintf (stderr, "   FS: to %3d.%3d FS Error - Unknown operation 0x%02x\n", net, stn, fsop);
			fs_error(server, reply_port, net, stn, 0xff, "FS Error");
		}
		break;

	}
}