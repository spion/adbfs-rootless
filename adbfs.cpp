/*
   @file
   @author  Calvin Tee (collectskin.com)
   @author  Sudarshan S. Chawathe (eip10.org)
   @version 0.1

   @section License

   BSD; see comments in main source files for details.

   @section Description

   A FUSE-based filesystem using the Android ADB interface.

   @mainpage

   adbFS: A FUSE-based filesystem using the Android ADB interface.

   Usage: To mount use

   @code adbfs mountpoint @endcode

   where mountpoint is a suitable directory. To unmount, use

   @code fusermount -u mountpoint @endcode

   as usual for FUSE.

   The above assumes you have a fairly standard Android development
   setup, with adb in the path, busybox available on the Android
   device, etc.  Everything is very lightly tested and a work in
   progress.  Read the source and use with caution.

*/

/*
 *      Software License Agreement (BSD License)
 *
 *      Copyright (c) 2010-2011, Calvin Tee (collectskin.com)
 *
 *      2011-12-25 Updated by Sudarshan S. Chawathe (chaw@eip10.org).
 *                 Fixed some problems due to filenames with spaces.
 *                 Added comments and miscellaneous small changes.
 *
 *      All rights reserved.
 *
 *      Redistribution and use in source and binary forms, with or without
 *      modification, are permitted provided that the following conditions are
 *      met:
 *
 *      * Redistributions of source code must retain the above copyright
 *        notice, this list of conditions and the following disclaimer.
 *      * Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following disclaimer
 *        in the documentation and/or other materials provided with the
 *        distribution.
 *      * Neither the name of the  nor the names of its
 *        contributors may be used to endorse or promote products derived from
 *        this software without specific prior written permission.
 *
 *      THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *      "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *      LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *      A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *      OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *      SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *      LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *      DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *      THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *      (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *      OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#define FUSE_USE_VERSION 30
#include "utils.h"
#include <unistd.h>

#include<stddef.h>
#include <execinfo.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <pwd.h>
#include <grp.h>
#include <time.h>

void handler(int sig) {
  void *array[10];
  size_t size;

  // get void*'s for all entries on the stack
  size = backtrace(array, 10);

  // print out all the frames to stderr
  fprintf(stderr, "Error: signal %d:\n", sig);
  backtrace_symbols_fd(array, size, 2);
  exit(1);
}


using namespace std;

void shell_escape_command(string&);
void adb_shell_escape_command(string&);
queue<string> adb_push(const string&, const string&);
queue<string> adb_pull(const string&, const string&);
queue<string> adb_shell(const string&, bool);
queue<string> shell(const string&);

static const char PERMISSION_ERR_MSG[] = ": Permission denied";

string tempDirPath;
map<string,fileCache> fileData;
void invalidateCache(const string& path) {
    cout << "invalidate cache " << path << endl;
    map<string, fileCache>::iterator it = fileData.find(path);
    if (it != fileData.end())
        fileData.erase(it);
}

map<int,bool> filePendingWrite;
map<string,bool> fileTruncated;

/**
   Custom options
 */

struct adb_config {
    bool rescan;
    bool shorttime;
    bool nochmod;
    bool nochown;
    bool noutimes;
};

static struct fuse_opt adb_opts[] = {
    { "rescan", offsetof(struct adb_config, rescan), true },
    { "shorttime", offsetof(struct adb_config, shorttime), true },
    { "nochmod", offsetof(struct adb_config, nochmod), true },
    { "nochown", offsetof(struct adb_config, nochown), true },
    { "noutimes", offsetof(struct adb_config, noutimes), true },
    FUSE_OPT_END
};

static struct adb_config adbfs_conf;

/**
   Return the result of executing the given command string, using
   exec_command, on the local host.

   @param command the command to execute.
   @see exec_command.
 */
queue<string> shell(const string& command)
{
    string actual_command;
    actual_command.assign(command);
    //shell_escape_command(actual_command);
    return exec_command(actual_command);
}

/**
   Return the result of executing the given command on the Android
   device using adb.

   The given string command is prefixed with "adb shell " to
   yield the adb command line.

   @param command the command to execute.
   @see exec_command.
   @todo perhaps avoid use of local shell to simplify escaping.
 */
queue<string> adb_shell(const string& command, bool getStderr = false)
{
    string actual_command;
    actual_command.assign(command);
    //adb_shell_escape_command(actual_command);
    actual_command.insert(0, "adb shell \"");
    actual_command.append("\" 2>/dev/null");
    if (getStderr) actual_command.append(" 2>&1");
    return exec_command(actual_command);
}

/**
   Modify, in place, the given string by escaping characters that are
   special to the shell.

   @param cmd the string to be escaped.
   @see adb_shell_escape_command.
   @todo check/simplify escaping.
 */
void shell_escape_command(string& cmd)
{
    string_replacer(cmd,"\\","\\\\");
    string_replacer(cmd,"'","\\'");
    string_replacer(cmd,"`","\\`");
}

/**
   Modify, in place, the given string by escaping characters that are
   special to the adb shell.

   @param cmd the string to be escaped.
   @see shell_escape_command.
   @todo check/simplify escaping.
 */
void adb_shell_escape_command(string& cmd)
{
    string_replacer(cmd,"\\","\\\\");
    string_replacer(cmd,"(","\\(");
    string_replacer(cmd,")","\\)");
    string_replacer(cmd,"'","\\'");
    string_replacer(cmd,"`","\\`");
    string_replacer(cmd,"|","\\|");
    string_replacer(cmd,"&","\\&");
    string_replacer(cmd,";","\\;");
    string_replacer(cmd,"<","\\<");
    string_replacer(cmd,">","\\>");
    string_replacer(cmd,"*","\\*");
    string_replacer(cmd,"#","\\#");
    string_replacer(cmd,"%","\\%");
    string_replacer(cmd,"=","\\=");
    string_replacer(cmd,"~","\\~");
    string_replacer(cmd,"/[0;0m","");
    string_replacer(cmd,"/[1;32m","");
    string_replacer(cmd,"/[1;34m","");
    string_replacer(cmd,"/[1;36m","");
}

/**
   Modify, in place, the given path string by escaping special characters.

   @param path the string to modify.
   @see shell_escape_command.
   @todo check/simplify escaping.
 */
void shell_escape_path(string &path)
{
  string_replacer(path, "'", "'\\''");
  string_replacer(path, "\"", "\\\"");
//  string_replacer(path, "`", "\\`");
}

/**
   Make a secure temporary directory for each mounted filesystem. Use with
   ANDROID_SERIAL environment variable to mount multiple phones at once.

   Also set up a callback to cleanup after ourselves on clean shutdown.
 */
void cleanupTmpDir(void) {
    string command = "rm -rf ";
    command.append(tempDirPath);
    shell(command);
}

void makeTmpDir(void) {
    char adbfsTemplate[]="/tmp/adbfs-XXXXXX";
    tempDirPath.assign(mkdtemp(adbfsTemplate));
    tempDirPath.append("/");
    atexit(&cleanupTmpDir);
}

/**
   Set a given string to an adb push or pull command with given paths.

   @param cmd string to which the adb command is written.
   @param push true for a push command, false for pull.
   @param local_path path on local host for push or pull command.
   @param remote_path path on remote device for push or pull command.
   @see adb_pull.
   @see adb_push.
 */
void adb_push_pull_cmd(string& cmd, const bool push,
		       const string& local_path, const string& remote_path)
{
    cmd.assign("adb ");
    cmd.append((push ? "push '" : "pull '"));
    cmd.append((push ? local_path : remote_path));
    cmd.append("' '");
    cmd.append((push ? remote_path : local_path));
    cmd.append("'");
}

/**
   Copy (using adb pull) a file from the Android device to the local
   host.

   @param remote_source Android-side file path to copy.
   @param local_destination local host-side destination path for copy.
   @return result of the "adb pull ..." executed using exec_command.
   @see adb_push.
   @see adb_push_pull_cmd.
   @todo perhaps avoid or simplify shell-escaping.
   @bug problems with files with spaces in filenames (adb bug?)
 */
queue<string> adb_pull(const string& remote_source,
		       const string& local_destination)
{
    string cmd;
    adb_push_pull_cmd(cmd, false, local_destination, remote_source);
    return exec_command(cmd);
}

/**
   Copy (using adb push) a file from the local host to the Android
   device. Very similar to adb_pull.

   @see adb_pull.
   @see adb_push_pull_cmd.
   @bug problems with files with spaces in filenames (adb bug?)
 */
queue<string> adb_push(const string& local_source,
		       const string& remote_destination)
{
    string cmd;
    adb_push_pull_cmd(cmd, true, local_source, remote_destination);
    queue<string> res = exec_command(cmd);
    invalidateCache(remote_destination);
    return res;
}

/**
   Tells Android to rescan the remote file for media changes.
 */
queue<string> adb_rescan_file(const string& remote_path)
{
    string cmd;
    cmd.assign("am broadcast -a android.intent.action.MEDIA_SCANNER_SCAN_FILE -d 'file://");
    cmd.append(remote_path);
    cmd.append("'");
    return adb_shell(cmd);
}

/**
   Tells Android to remove the remote directory from its media database.
 */
queue<string> adb_rescan_dir_removed(const string& remote_path)
{
    string cmd;
    cmd.assign("am broadcast -a android.intent.action.MEDIA_UNMOUNTED -d 'file://");
    cmd.append(remote_path);
    cmd.append("'");
    return adb_shell(cmd);
}

/**
   adbFS implementation of FUSE interface function fuse_operations.getattr.
   @todo check shell escaping.
 */



int strmode_to_rawmode(const string& str) {
    int fmode = 0;
    switch (str[0]) {
    case 's': fmode |= S_IFSOCK; break;
    case 'l': fmode |= S_IFLNK; break;
    case '-': fmode |= S_IFREG; break;
    case 'd': fmode |= S_IFDIR; break;
    case 'b': fmode |= S_IFBLK; break;
    case 'c': fmode |= S_IFCHR; break;
    case 'p': fmode |= S_IFIFO; break;
    }

    if (str[1] == 'r') fmode |= S_IRUSR;
    if (str[2] == 'w') fmode |= S_IWUSR;
    switch (str[3]) {
    case 'x': fmode |= S_IXUSR; break;
    case 's': fmode |= S_ISUID | S_IXUSR; break;
    case 'S': fmode |= S_ISUID; break;
    }

    if (str[4] == 'r') fmode |= S_IRGRP;
    if (str[5] == 'w') fmode |= S_IWGRP;
    switch (str[6]) {
    case 'x': fmode |= S_IXGRP; break;
    case 's': fmode |= S_ISGID | S_IXGRP; break;
    case 'S': fmode |= S_ISGID; break;
    }

    if (str[7] == 'r') fmode |= S_IROTH;
    if (str[8] == 'w') fmode |= S_IWOTH;
    switch (str[9]) {
    case 'x': fmode |= S_IXOTH; break;
    case 't': fmode |= S_ISVTX | S_IXOTH; break;
    case 'T': fmode |= S_ISVTX; break;
    }

    return fmode;

        // In octal,
        //     // 40XXX is folder, 100xxx is file
        //         // xxx is regular mode e.g. 755 = -rwxr-xr-x
        //

}

// Heuristic to determine whether the output of ls produced
// an actual file
bool is_valid_ls_output(const string& file) {
  /* The specific error messages we are looking for (from the android source)-
     (in listdir) "opendir failed, strerror"
     (in show_total_size) "stat failed on filename, strerror"
     (in listfile_size) "lstat 'filename' failed: strerror"

     Thus, we can abuse this a little and just make sure that the second
     character is either "r" or "-", and assume it's an error otherwise.

     To eliminate cases such as /rfile: no such file or directory from
     producing false-positives, we also check whether the first character
     is a slash

     It'd be really nice if we could actually take the strerrors and convert
     them back to codes, but I fear that involves undoing localization.
  */
  if (file.size() < 2) return false;
  if (file[0] == '/') return false;
  if (file[1] != 'r' && file[1] != '-') return false;
  return true;
}

static void *adb_init(struct fuse_conn_info *conn,
                        struct fuse_config *cfg)
{
	//cfg->auto_cache = 1;
	return NULL;
}

static int adb_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi)
{
	(void)fi;
    cout << "adb_getattr" << endl;
    int res = 0;
    struct passwd * foruid;
    struct group * forgid;
    memset(stbuf, 0, sizeof(struct stat));
    queue<string> output;
    string path_string;
    path_string.assign(path);
    shell_escape_path(path_string);
    // TODO /caching?
    //
    vector<string> output_chunk;
    if (fileData.find(path_string) ==  fileData.end()
	|| fileData[path_string].timestamp + 30 < time(NULL)) {
        string command;
        if (!adbfs_conf.shorttime) {
            command  = "ls -ll -a -d '";
        } else {
            command = "ls -l -a -d '";
        }
        command.append(path_string);
        command.append("'");
        output = adb_shell(command, true);
        if (output.empty()) return -EAGAIN; /* no phone */
        // error format: "/sbin/healthd: Permission denied"
        if (
            output.front().length() > sizeof(PERMISSION_ERR_MSG) &&
            (!output.front().compare(output.front().length() - sizeof(PERMISSION_ERR_MSG) + 1,
                                    sizeof(PERMISSION_ERR_MSG) - 1, PERMISSION_ERR_MSG)))
        {
            fileData[path_string].statOutput.erase();
        } else {
            output_chunk = make_array(output.front());
            fileData[path_string].statOutput = output.front();
        }
        fileData[path_string].timestamp = time(NULL);
    } else{
        output_chunk = make_array(fileData[path_string].statOutput);
        cout << "from cache " << path << "\n";
    }
    if (fileData[path_string].statOutput.empty()) {
        // return empty structure - file exists, but no info available
        stbuf->st_mode = S_IFREG;
        return res;
    }

    if(!is_valid_ls_output(output_chunk[0])) {
        return -ENOENT;
    }

    //
    // ls -lad explained
    // -rw-rw-r-- root     sdcard_rw   763362 2012-06-22 02:16 file.html
    //
    // Alternative
    // -rw-r--r--   1 root   root      5905 1970-01-01 01:00 ueventd.rc
    //stbuf->st_dev = atoi(output_chunk[1].c_str());     /* ID of device containing file */
    //
    // In octal,
    // 40XXX is folder, 100xxx is file
    // xxx is regular mode e.g. 755 = rwxr-xr-x
    //

    stbuf->st_ino = 1;      /* inode number, fake. */

    stbuf->st_mode = strmode_to_rawmode(output_chunk[0]); // | 0700

    int uid_offset = 0;

    stbuf->st_nlink = atoi(output_chunk[1].c_str());
    if (stbuf->st_nlink > 0) uid_offset = 1;
    else stbuf->st_nlink = 1;

    foruid = getpwnam(output_chunk[uid_offset + 1].c_str());
    if (foruid)
	    stbuf->st_uid = foruid->pw_uid;     /* user ID of owner */
    else
	    stbuf->st_uid = 98; /* 98 has been chosen (poorly) so that it doesn't map to anything */

    forgid = getgrnam(output_chunk[uid_offset + 2].c_str());
    if (forgid)
	    stbuf->st_gid = forgid->gr_gid;     /* group ID of owner */
    else
	    stbuf->st_gid = 98;

    //unsigned int device_id;
    //xtoi(output_chunk[6].c_str(),&device_id);
    //stbuf->st_rdev = device_id;    // device ID (if special file)

    int iDate;

    switch (stbuf->st_mode & S_IFMT) {
    case S_IFBLK:
    case S_IFCHR:
	    stbuf->st_rdev = atoi(output_chunk[uid_offset + 3].c_str()) * 256 +
                         atoi(output_chunk[uid_offset + 4].c_str());
	    stbuf->st_size = 0;
	    iDate = uid_offset + 5;
	    break;

	    break;

    case S_IFREG:
	    stbuf->st_size = atol(output_chunk[uid_offset + 3].c_str());    /* total size, in bytes */
	    iDate = uid_offset + 4;
	    break;

    default:
    case S_IFSOCK:
    case S_IFIFO:
    case S_IFLNK:
    case S_IFDIR:
	    stbuf->st_size = 0;
	    iDate = uid_offset + 3;
        if (output_chunk[iDate].find_first_of("-") == string::npos) ++iDate;
	    break;
    }

    // du calculates sizes based on number of 512b blocks
    stbuf->st_blksize = 512;
    stbuf->st_blocks = (stbuf->st_size + 256) / 512;

    //for (int k = 0; k < output_chunk.size(); ++k) cout << output_chunk[k] << " ";
    //cout << endl;

    vector<string> ymd = make_array(output_chunk[iDate], "-");
    vector<string> hm = make_array(output_chunk[iDate + 1], ":");


	if (ymd.size() == 3 && hm.size() == 2)
	{
			
	    //for (int k = 0; k < ymd.size(); ++k) cout << ymd[k] << " ";
	    //cout << endl;
	    //for (int k = 0; k <  hm.size(); ++k) cout <<  hm[k] << " ";
	    //cout << endl;
	    struct tm ftime{};
	    ftime.tm_year = atoi(ymd[0].c_str()) - 1900;
	    ftime.tm_mon  = atoi(ymd[1].c_str()) - 1;
	    ftime.tm_mday = atoi(ymd[2].c_str());
	    ftime.tm_hour = atoi(hm[0].c_str());
	    ftime.tm_min  = atoi(hm[1].c_str());
		if (!adbfs_conf.shorttime) {
            vector<string> s_ns = make_array(hm[2], ".");
			ftime.tm_sec  = atoi(s_ns[0].c_str());
//        for (int k = 0; k <  s_ns.size(); ++k) cout <<  s_ns[k] << " ";
//        cout << endl;
		} else {
			ftime.tm_sec  = 0;
		}

	    ftime.tm_isdst = -1;
	    time_t now = mktime(&ftime);
	    //cout << "after mktime" << endl;
	
	    //long now = time(0);
	
	    stbuf->st_atime = now;   /* time of last access */
	    //stbuf->st_atime = atol(output_chunk[11].c_str());   /* time of last access */
	    stbuf->st_mtime = now;   /* time of last modification */
	    //stbuf->st_mtime = atol(output_chunk[12].c_str());   /* time of last modification */
	    stbuf->st_ctime = now;   /* time of last status change */
	    //stbuf->st_ctime = atol(output_chunk[13].c_str());   /* time of last status change */
	}
    return res;
}


size_t find_nth(int n, const string& substr, const string& corpus) {
    size_t p = 0;
    while (n--) {
        if ((( p = corpus.find_first_of(substr, p) )) == string::npos) return string::npos;
        p = corpus.find_first_not_of(substr, p);
    }
    return p;
};


/**
   adbFS implementation of FUSE interface function fuse_operations.readdir.
   @todo check shell escaping.
 */
static int adb_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
    off_t offset, struct fuse_file_info *fi, enum fuse_readdir_flags flags)
{
    (void) offset;
    (void) fi;
    (void) flags;
    string path_string;
    string local_path_string;
    path_string.assign(path);
    local_path_string = tempDirPath;
    string_replacer(path_string,"/","-");
    local_path_string.append(path_string);
    path_string.assign(path);

    shell_escape_path(path_string);

    queue<string> output;
    string command;
    if (!adbfs_conf.shorttime) {
        command = "ls -ll -a '";
    } else {
        command = "ls -l -a '";
    }
    command.append(path_string);
    command.append("'");
    output = adb_shell(command);

    /* cannot tell between "no phone" and "empty directory" */
    cout << "found files: " << output.size() << endl;
    while (output.size() > 0) {
        // skip lines too short to process (should not happen)
        if (output.front().length() >= 3) {
            // we can get e.g. "permission denied" during listing, need to check every line separately
            if (!is_valid_ls_output(output.front())) {
                // error format: "lstat '//efs' failed: Permission denied"
                if (
                     output.front().length() > sizeof(PERMISSION_ERR_MSG) &&
                     (!output.front().compare(output.front().length() - sizeof(PERMISSION_ERR_MSG) + 1,
                                            sizeof(PERMISSION_ERR_MSG) - 1, PERMISSION_ERR_MSG))) {
                    size_t nameStart = output.front().rfind("/") + 1;
                    const string& fname_l = output.front().substr(nameStart, output.front().find("' ") - nameStart);
                    cout << "Adding file:" << fname_l << ":" << endl;
                    filler(buf, fname_l.c_str(), NULL, 0, (fuse_fill_dir_flags)0);
                    const string& path_string_c = path_string
                        + (path_string == "/" ? "" : "/") + fname_l;

                    cout << "caching " << path_string_c << " = " << output.front() <<  endl;
                    fileData[path_string_c].statOutput.erase();
                    fileData[path_string_c].timestamp = time(NULL);
                    cout << "cached " << endl;
                }
            } else {
                // Start of filename = `ls -la` time separator + 4
                size_t nameStart = output.front().find_first_of(":") + 4;
                if (!adbfs_conf.shorttime) {
                    nameStart = output.front().find_first_of(":") + 23;
                } else {
                    nameStart = output.front().find_first_of(":") + 4;
                }
                const string& fname_l = output.front().substr(nameStart);
                const string fname_n = fname_l.substr(0, fname_l.find(" -> "));
                cout << "Adding file:" << fname_n <<":" << endl;
                filler(buf, fname_n.c_str(), NULL, 0, (fuse_fill_dir_flags)0);
                const string path_string_c = path_string
                    + (path_string == "/" ? "" : "/") + fname_n;

                cout << "caching " << path_string_c << " = " << output.front() <<  endl;
                fileData[path_string_c].statOutput = output.front();
                fileData[path_string_c].timestamp = time(NULL);
                cout << "cached " << endl;
            }
        }
        output.pop();
    }
    cout << "done with found files" << endl;


    return 0;
}


static int adb_open(const char *path, struct fuse_file_info *fi)
{
    string path_string;
    string local_path_string;
    path_string.assign(path);
    local_path_string = tempDirPath;
    string_replacer(path_string,"/","-");
    local_path_string.append(path_string);

    string filehandle_path = local_path_string;

    path_string.assign(path);
    shell_escape_path(path_string);
    shell_escape_path(local_path_string);

    cout << "-- adb_open --" << path_string << " " << local_path_string << "\n";
    if (!fileTruncated[path_string]){
        queue<string> output;
        string command;
        if (!adbfs_conf.shorttime) {
            command = "ls -ll -a -d '";
        } else {
            command = "ls -l -a -d '";
        }
        command.append(path_string);
        command.append("'");
        cout << command<<"\n";
        output = adb_shell(command);
        if (output.empty()) {
            return -ENOENT;
        }
        vector<string> output_chunk = make_array(output.front());
        if (!is_valid_ls_output(output_chunk[0])) {
          return -ENOENT;
        }
        path_string.assign(path);
        local_path_string = tempDirPath;
        string_replacer(path_string,"/","-");
        local_path_string.append(path_string);
        path_string.assign(path);
        shell_escape_path(path_string);
        shell_escape_path(local_path_string);
        adb_pull(path_string,local_path_string);
    } else {
        fileTruncated[path_string] = false;
    }

    fi->fh = open(filehandle_path.c_str(), fi->flags);

    return 0;
}

static int adb_read(const char *path, char *buf, size_t size, off_t offset,
    struct fuse_file_info *fi)
{
    int fd;
    int res;
    fd = fi->fh; //open(local_path_string.c_str(), O_RDWR);
    if(fd == -1)
        return -errno;
    res = pread(fd, buf, size, offset);
    //close(fd);
    if(res == -1)
        res = -errno;

    return res;
}

static int adb_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    //string path_string;
    //string local_path_string;
    //path_string.assign(path);
    //shell_escape_path(path_string);

    int fd = fi->fh; //open(local_path_string.c_str(), O_CREAT|O_RDWR|O_TRUNC);

    filePendingWrite[fd] = true;

    int res = pwrite(fd, buf, size, offset);
    //close(fd);
    //adb_push(local_path_string,path_string);
    //adb_shell("sync");
    if (res == -1)
        res = -errno;
    return res;
}


static int adb_flush(const char *path, struct fuse_file_info *fi) {
    string path_string;
    string local_path_string;
    path_string.assign(path);
    local_path_string = tempDirPath;
    string_replacer(path_string,"/","-");
    local_path_string.append(path_string);
    path_string.assign(path);

    shell_escape_path(path_string);
    shell_escape_path(local_path_string);

    int flags = fi->flags;
    int fd = fi->fh;
    cout << "flag is: "<< flags <<"\n";
    invalidateCache(path_string);
    if (filePendingWrite[fd]) {
        filePendingWrite[fd] = false;
        adb_push(local_path_string, path_string);
        adb_shell("sync");
        if (adbfs_conf.rescan) adb_rescan_file(path_string);
    }
    return 0;
}

static int adb_release(const char *path, struct fuse_file_info *fi) {
    // just like in the other functions
    string path_string;
    string local_path_string;
    path_string.assign(path);
    local_path_string = tempDirPath;
    string_replacer(path_string,"/","-");
    local_path_string.append(path_string);
    path_string.assign(path);

    // untouched
    int fd = fi->fh;
    filePendingWrite.erase(filePendingWrite.find(fd));
    close(fd);
    
    // remove local copy
    unlink(local_path_string.c_str());    
    return 0;
}

static int adb_access(const char *path, int mask) {
    //###cout << "###access[path=" << path << "]" <<  endl;
    return 0;
}

static int adb_utimens(const char *path, const struct timespec ts[2], struct fuse_file_info *fi) {
    (void)fi;
    string path_string;
    path_string.assign(path);
    fileData[path_string].timestamp = fileData[path_string].timestamp + 50;

    shell_escape_path(path_string);

    string command;
    if (adbfs_conf.noutimes) {
        command = "touch '";
        command.append(path_string);
        command.append("'");
    } else {
        char timebuf[32];
        struct tm _tm;
        localtime_r(&(ts[1].tv_sec), &_tm);
        strftime(timebuf, sizeof(timebuf), "%Y%m%d%H%M.%S", &_tm);
        command = "touch -am -t ";
        command.append(timebuf);
        command.append(" '");
        command.append(path_string);
        command.append("'");
    }
    cout << command <<"\n";
    adb_shell(command);

    // If we forgot to mount -o rescan then we can remount and touch to trigger the scan.
    if (adbfs_conf.rescan) adb_rescan_file(path_string);

    return 0;
}

static int adb_truncate(const char *path, off_t size, struct fuse_file_info *fi) {
    (void)fi;
    string path_string;
    string local_path_string;
    path_string.assign(path);
    fileData[path_string].timestamp = fileData[path_string].timestamp + 50;
    local_path_string = tempDirPath;
    string_replacer(path_string,"/","-");
    local_path_string.append(path_string);
    path_string.assign(path);
    shell_escape_path(path_string);
    shell_escape_path(local_path_string);


    queue<string> output;
    cout << "adb_truncate" << endl;
    string command;
    if (!adbfs_conf.shorttime) {
        command = "ls -ll -a -d '";
    } else {
        command = "ls -l -a -d '";
    }
    command.append(path_string);
    command.append("'");
    cout << command << "\n";
    output = adb_shell(command);
    if (output.empty()) {
        return -ENOENT;
    }
    vector<string> output_chunk = make_array(output.front());
    if (output_chunk[0][0] == '/'){
        adb_pull(path_string,local_path_string);
    }

    fileTruncated[path_string] = true;

    invalidateCache(path_string);

    cout << "truncate[path=" << local_path_string << "][size=" << size << "]" << endl;

    return truncate(local_path_string.c_str(),size);
}

static int adb_mknod(const char *path, mode_t mode, dev_t rdev) {
    string path_string;
    string local_path_string;
    path_string.assign(path);
    local_path_string = tempDirPath;
    string_replacer(path_string,"/","-");
    local_path_string.append(path_string);
    path_string.assign(path);

    cout << "mknod for " << local_path_string << "\n";
    mknod(local_path_string.c_str(),mode, rdev);

    shell_escape_path(path_string);
    shell_escape_path(local_path_string);

    adb_push(local_path_string,path_string);
    adb_shell("sync");

    invalidateCache(path_string);

    return 0;
}

static int adb_mkdir(const char *path, mode_t mode) {
    string path_string;
    string local_path_string;
    path_string.assign(path);
    fileData[path_string].timestamp = fileData[path_string].timestamp + 50;
    local_path_string = tempDirPath;
    string_replacer(path_string,"/","-");
    local_path_string.append(path_string);
    path_string.assign(path);

    shell_escape_path(path_string);

    string command;
    command.assign("mkdir '");
    command.append(path_string);
    command.append("'");
    adb_shell(command);
    invalidateCache(path_string);
    return 0;
}

static int adb_rename(const char *from, const char *to, unsigned int flags) {

	if(flags != 0)
		return -EINVAL;

    string local_from_string,local_to_string = tempDirPath;

    string from_string = string(from), to_string = string(to);


    local_from_string.append(from);
    local_to_string.append(to);

    shell_escape_path(local_from_string);
    shell_escape_path(local_to_string);

    shell_escape_path(from_string);
    shell_escape_path(to_string);


    string command = "mv '";
    command.append(from_string);
    command.append("' '");
    command.append(to_string);
    command.append("'");
    cout << "Renaming " << from << " to " << to <<"\n";
    adb_shell(command);
    if (adbfs_conf.rescan) {
        adb_rescan_file(from);
        adb_rescan_file(to);
    }
    invalidateCache(string(from));
    invalidateCache(string(to));
    return 0;
}

static int adb_rmdir(const char *path) {
    string path_string;
    string local_path_string;
    path_string.assign(path);
    fileData[path_string].timestamp = fileData[path_string].timestamp + 50;
    local_path_string = tempDirPath;
    string_replacer(path_string,"/","-");
    local_path_string.append(path_string);
    path_string.assign(path);

    shell_escape_path(path_string);
    shell_escape_path(local_path_string);


    string command = "rmdir '";
    command.append(path_string);
    command.append("'");
    adb_shell(command);
    if (adbfs_conf.rescan) adb_rescan_dir_removed(path_string);
    invalidateCache(path_string);

    //rmdir(local_path_string.c_str());
    return 0;
}

static int adb_unlink(const char *path) {
    string path_string;
    string local_path_string;
    path_string.assign(path);
    fileData[path_string].timestamp = fileData[path_string].timestamp + 50;
    local_path_string = tempDirPath;
    string_replacer(path_string,"/","-");
    local_path_string.append(path_string);
    path_string.assign(path);

    shell_escape_path(path_string);
    shell_escape_path(local_path_string);

    string command = "rm '";
    command.append(path_string);
    command.append("'");
    adb_shell(command);
    if (adbfs_conf.rescan) adb_rescan_file(path_string);
    invalidateCache(path_string);
    unlink(local_path_string.c_str());
    return 0;
}

static int adb_readlink(const char *path, char *buf, size_t size)
{
    cout << "adb_readlink" << endl;
    string path_string(path);
    shell_escape_path(path_string);

    queue<string> output;

    // get the number of slashes in the path
    int num_slashes, ii;
    for (num_slashes = ii = 0; ii < strlen(path); ii++)
        if (path[ii] == '/')
            num_slashes++;
    if (num_slashes >= 1) num_slashes--;

    if (fileData.find(path_string) ==  fileData.end()
	|| fileData[path_string].timestamp + 30 < time(NULL)) {
        string command;
        if (!adbfs_conf.shorttime) {
            command = "ls -ll -a -d '";
        } else {
            command = "ls -l -a -d '";
        }
        command.append(path_string);
        command.append("'");
        output = adb_shell(command);
        if (output.empty())
            return -EINVAL;
        // error format: "/sbin/healthd: Permission denied"

        if ((output.front().length() > sizeof(PERMISSION_ERR_MSG)) &&
           (!output.front().compare(output.front().length() - sizeof(PERMISSION_ERR_MSG) + 1,
                                    sizeof(PERMISSION_ERR_MSG) - 1, PERMISSION_ERR_MSG)))
        {
            fileData[path_string].statOutput.erase();
        } else {
            fileData[path_string].statOutput = output.front();
        }
        fileData[path_string].timestamp = time(NULL);
    } else{
        cout << "from cache " << path << "\n";
    }
    string &res = fileData[path_string].statOutput;
    if (res.empty()) {
        // file exists, but no info available
        return -EINVAL;
    }
    if (!is_valid_ls_output(res)) {
      return -ENOENT;
    }
    cout << "adb_readlink " << res << endl;
    size_t pos = res.find(" -> ");
    if(pos == string::npos)
       return -EINVAL;
    pos+=4;
    size_t my_size = res.size();
    buf[0] = 0;
    if (res[pos] == '/') {
	    while(res[pos] == '/')
		    ++pos;
	    my_size += 3 * num_slashes - pos;
	    if(my_size >= size)
		    return -ENOSYS;
	    for (;num_slashes;num_slashes--) {
		    strncat(buf,"../",size);
	    }
    }
    if(my_size >= size)
	    return -ENOSYS;
    strncat(buf, res.c_str() + pos,size);
    return 0;
}


static int adb_statfs(const char *path, struct statvfs *buf)
{
    //cout << "statfs called: " << path << "\n";
    vector<int> blocks_info;

    // FIXME: cause crash
    /*
    string command = "df '";
    command.append(path);
    command.append("'");
    auto output_queue = adb_shell(command);
    if (!output_queue.empty())
        output_queue.pop();
    if (!output_queue.empty() && output_queue.front().size() > 0)
    {
        auto output_arr = make_array(output_queue.front());
        for (int i=0; i<output_arr.size(); i++)
        {
            string s = output_arr[i];
            string::const_iterator it = s.begin();
            while (it != s.end() && std::isdigit(*it)) ++it;
            if (!s.empty() && it == s.end())
                blocks_info.push_back(atoi(s.c_str()));
        }
    }
    */

	buf->f_namemax = 255;
    buf->f_bsize = 4096;
    buf->f_frsize = buf->f_bsize;
    if (blocks_info.size() == 3)
    {
        buf->f_blocks = blocks_info[0]/4;
        buf->f_bfree = buf->f_bavail = blocks_info[2]/4;
        //buf->f_bfree = buf->f_bavail = buf->f_blocks - blocks_info[1]/4;
    }
    else
    {
        buf->f_blocks = buf->f_bfree = buf->f_bavail =
            1000ULL * 1024 * 1024 * 1024 / buf->f_frsize;
    }
	buf->f_files = buf->f_ffree = 1000000000;
	return 0;
}

int adb_chmod_stub(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    //adbfs_conf.nochown
    (void)fi;
    (void)path;
    (void)mode;

    return 0;
}

int adb_chown_stub(const char *path, uid_t uid, gid_t gid, struct fuse_file_info *fi)
{
    //adbfs_conf.nochmod
    (void)fi;
    (void)path;
    (void)uid;
    (void)gid;

    return 0;
}

/**
   Main struct for FUSE interface.
   Set up the fuse_operations struct adbfs_oper using above adb_*
   functions
 */
static struct fuse_operations adbfs_oper = {
    .getattr    = adb_getattr,
    .readlink   = adb_readlink,
    .mknod      = adb_mknod,
    .mkdir      = adb_mkdir,
    .unlink     = adb_unlink,
    .rmdir      = adb_rmdir,
    .rename     = adb_rename,
    .chmod      = adb_chmod_stub,
    .chown      = adb_chown_stub,
    .truncate   = adb_truncate,
    .open       = adb_open,
    .read       = adb_read,
    .write      = adb_write,
    .statfs     = adb_statfs,
    .flush      = adb_flush,
    .release    = adb_release,
    .readdir    = adb_readdir,
    .init       = adb_init,
    .access     = adb_access,
    .utimens    = adb_utimens,
};

/**
 * call fuse_main to manage things.
   @see fuse_main in fuse.h.
 */
int main(int argc, char *argv[])
{
    signal(SIGSEGV, handler);   // install our handler
    makeTmpDir();

    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
    memset(&adbfs_conf, 0, sizeof(adbfs_conf));
    fuse_opt_parse(&args, &adbfs_conf, adb_opts, NULL);

    adb_shell("ls");

    return fuse_main(args.argc, args.argv, &adbfs_oper, NULL);
}
