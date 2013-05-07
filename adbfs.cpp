/**
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
 
#define FUSE_USE_VERSION 26
#include "utils.h"

#include <execinfo.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <pwd.h>
#include <grp.h>

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
queue<string> adb_shell(const string&);
queue<string> shell(const string&);

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
   Return the result of executing the given command string, using
   exec_command, on the local host.

   @param command the command to execute.
   @see exec_command.
 */
queue<string> shell(const string& command)
{
    string actual_command;
    actual_command.assign(command);
    shell_escape_command(actual_command);
    return exec_command(actual_command);
}

/**
   Return the result of executing the given command on the Android
   device using adb.

   The given string command is prefixed with "adb shell busybox " to
   yield the adb command line.

   @param command the command to execute.
   @see exec_command.
   @todo perhaps avoid use of local shell to simplify escaping.
 */
queue<string> adb_shell(const string& command)
{
    string actual_command;
    actual_command.assign(command);
    adb_shell_escape_command(actual_command);
    actual_command.insert(0, "adb shell ");
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
  //string_replacer(path, " ", "\\ ");
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



static int adb_getattr(const char *path, struct stat *stbuf)
{

    int res = 0;
    struct passwd * foruid;
    struct group * forgid;
    memset(stbuf, 0, sizeof(struct stat));
    queue<string> output;
    string path_string;
    path_string.assign(path);

    // TODO /caching?
    //
    vector<string> output_chunk;
    if (fileData.find(path_string) ==  fileData.end() 
	|| fileData[path_string].timestamp + 30 < time(NULL)) {
        string command = "ls -l -a -d \"";
        command.append(path_string);
        command.append("\"");
        output = adb_shell(command);
        if (output.empty()) return -EAGAIN; /* no phone */
        output_chunk = make_array(output.front());
        fileData[path_string].statOutput = output_chunk;
        fileData[path_string].timestamp = time(NULL);
    } else{
        output_chunk = fileData[path_string].statOutput;
        cout << "from cache " << path << "\n";
    }
    //vector<string> output_chunk = make_array(output.front());
    if (output_chunk[0][0] == '/'){
        return -ENOENT;
    }
    /*
       stat -t Explained:
       file name (%n)
       total size (%s)
       number of blocks (%b)
       raw mode in hex (%f)
       UID of owner (%u)
       GID of file (%g)
       device number in hex (%D)
       inode number (%i)
       number of hard links (%h)
       major devide type in hex (%t)
       minor device type in hex (%T)
       last access time as seconds since the Unix Epoch (%X)
       last modification as seconds since the Unix Epoch (%Y)
       last change as seconds since the Unix Epoch (%Z)
       I/O block size (%o)
       */
    // 
    // ls -lad explained
    // -rw-rw-r-- root     sdcard_rw   763362 2012-06-22 02:16 fajlot.html
    //
    //stbuf->st_dev = atoi(output_chunk[1].c_str());     /* ID of device containing file */
    //
    // In octal,
    // 40XXX is folder, 100xxx is file
    // xxx is regular mode e.g. 755 = rwxr-xr-x
    //

    stbuf->st_ino = 1; // atoi(output_chunk[7].c_str());     /* inode number */
    //stbuf->st_mode = raw_mode | 0700;    /* protection */
    stbuf->st_mode = strmode_to_rawmode(output_chunk[0]);

    stbuf->st_nlink = 1;   /* number of hard links */

    foruid = getpwnam(output_chunk[1].c_str());
    if (foruid)
	    stbuf->st_uid = foruid->pw_uid;     /* user ID of owner */
    else
	    stbuf->st_uid = 98; /* 98 has been chosen (poorly) so that it doesn't map to anything */

    forgid = getgrnam(output_chunk[2].c_str());
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
	    stbuf->st_rdev = atoi(output_chunk[3].c_str()) * 256 + atoi(output_chunk[4].c_str());
	    stbuf->st_size = 0;
	    iDate = 5;
	    break;

	    break;

    case S_IFREG:
	    stbuf->st_size = atoi(output_chunk[3].c_str());    /* total size, in bytes */
	    iDate = 4;
	    break;

    default:
    case S_IFSOCK:
    case S_IFIFO:
    case S_IFLNK:
    case S_IFDIR:
	    stbuf->st_size = 0;
	    iDate = 3;
	    break;
    }

    stbuf->st_blksize = 0; // TODO: THIS IS SMELLY
    stbuf->st_blocks = 1;

    //for (int k = 0; k < output_chunk.size(); ++k) cout << output_chunk[k] << " ";    
    //cout << endl;
    

    vector<string> ymd = make_array(output_chunk[iDate], "-");
    vector<string> hm = make_array(output_chunk[iDate + 1], ":");


    //for (int k = 0; k < ymd.size(); ++k) cout << ymd[k] << " ";
    //cout << endl;
    //for (int k = 0; k <  hm.size(); ++k) cout <<  hm[k] << " ";
    //cout << endl;
    struct tm ftime;
    ftime.tm_year = atoi(ymd[0].c_str()) - 1900;
    ftime.tm_mon  = atoi(ymd[1].c_str()) - 1;
    ftime.tm_mday = atoi(ymd[2].c_str());
    ftime.tm_hour = atoi(hm[0].c_str());
    ftime.tm_min  = atoi(hm[1].c_str());
    ftime.tm_sec  = 0;
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
    return res;
}


size_t find_nth(int n, const string& substr, const string& corpus) {
    size_t p = 0;
    while (n--) {
        if ((( p = corpus.find_first_of(substr, p) )) == string::npos) return string::npos;
        p = corpus.find_first_not_of(substr, p); 
    }   
    return p;
}


/**
   adbFS implementation of FUSE interface function fuse_operations.readdir.
   @todo check shell escaping.
 */
static int adb_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
    off_t offset, struct fuse_file_info *fi)
{
    (void) offset;
    (void) fi;
    string path_string;
    string local_path_string;
    path_string.assign(path);
    local_path_string = tempDirPath;
    string_replacer(path_string,"/","-");
    local_path_string.append(path_string);
    path_string.assign(path);

    queue<string> output;
    string command = "ls -l -a \"";
    command.append(path_string);
    command.append("\"");
    output = adb_shell(command);
    if (!output.size()) return 0;
    /* cannot tell between "no phone" and "empty directory" */
    vector<string> output_chunk = make_array(output.front());
    /* The specific error messages we are looking for (from the android source)-
       (in listdir) "opendir failed, strerror"
       (in show_total_size) "stat failed on filename, strerror"
       (in listfile_size) "lstat 'filename' failed: strerror"

       Thus, we can abuse this a little and just make sure that the second
       character is either "r" or "-", and assume it's an error otherwise.

       It'd be really nice if we could actually take the strerrors and convert
       them back to codes, but I fear that involves undoing localization.
    */
    if (output.front().length() < 3) return -ENOENT;
    if (output.front().c_str()[1] != 'r' &&
        output.front().c_str()[1] != '-') return -ENOENT;

    while (output.size() > 0) {
        const string& fname_l_t = output.front().substr(54); 
        const string& fname_l = fname_l_t.substr(find_nth(1, " ",fname_l_t));
        const string& fname_n = fname_l.substr(0, fname_l.find(" -> "));
        filler(buf, fname_n.c_str(), NULL, 0);
        const string& path_string_c = path_string 
            + (path_string == "/" ? "" : "/") + fname_n;

        cout << "caching " << path_string_c << " = " << output.front() <<  endl;
        fileData[path_string_c].statOutput = make_array(output.front());
        fileData[path_string_c].timestamp = time(NULL);
        cout << "cached " << endl;
        output.pop();

    }


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
    path_string.assign(path);
    cout << "-- " << path_string << " " << local_path_string << "\n";
    if (!fileTruncated[path_string]){
        queue<string> output;
        string command = "ls -l -a -d \"";
        command.append(path_string);
        command.append("\"");
        cout << command<<"\n";
        output = adb_shell(command);
        vector<string> output_chunk = make_array(output.front());
        if (output_chunk[0][0] == '/') {
            return -ENOENT;
        }
        path_string.assign(path);
        local_path_string = tempDirPath;
        string_replacer(path_string,"/","-");
        local_path_string.append(path_string);
        shell_escape_path(local_path_string);
        path_string.assign(path);
        adb_pull(path_string,local_path_string);
    } else {
        fileTruncated[path_string] = false;
    }

    fi->fh = open(local_path_string.c_str(), fi->flags);

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

    return size;
}

static int adb_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    string path_string;
    string local_path_string;
    path_string.assign(path);
    shell_escape_path(local_path_string);

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
    int flags = fi->flags;
    int fd = fi->fh;
    cout << "flag is: "<< flags <<"\n";
    invalidateCache(path_string);
    if (filePendingWrite[fd]) {
        filePendingWrite[fd] = false;
        adb_push(local_path_string,path_string);
        adb_shell("sync");
    }
    return 0;
}

static int adb_release(const char *path, struct fuse_file_info *fi) {
    int fd = fi->fh;
    filePendingWrite.erase(filePendingWrite.find(fd));
    close(fd);
    return 0;
}

static int adb_access(const char *path, int mask) {
    //###cout << "###access[path=" << path << "]" <<  endl;
    return 0;
}

static int adb_utimens(const char *path, const struct timespec ts[2]) {
    string path_string;
    string local_path_string;
    path_string.assign(path);
    fileData[path_string].timestamp = fileData[path_string].timestamp + 50;
    local_path_string = tempDirPath;
    string_replacer(path_string,"/","-");
    local_path_string.append(path_string);
    path_string.assign(path);

    queue<string> output;
    string command = "touch \"";
    command.append(path_string);
    command.append("\"");
    cout << command<<"\n";
    adb_shell(command);

    return 0;
}

static int adb_truncate(const char *path, off_t size) {
    string path_string;
    string local_path_string;
    path_string.assign(path);
    fileData[path_string].timestamp = fileData[path_string].timestamp + 50;
    local_path_string = tempDirPath;
    string_replacer(path_string,"/","-");
    local_path_string.append(path_string);
    path_string.assign(path);

    queue<string> output;
    string command = "ls -l -a -d \"";
    command.append(path_string);
    command.append("\"");
    cout << command<<"\n";
    output = adb_shell(command);
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
    string command;
    command.assign("mkdir '");
    command.append(path_string);
    command.append("'");
    adb_shell(command);
    invalidateCache(path_string);
    return 0;
}

static int adb_rename(const char *from, const char *to) {
    string local_from_string,local_to_string = tempDirPath;

    local_from_string.append(from);
    local_to_string.append(to);
    string command = "mv '";
    command.append(from);
    command.append("' '");
    command.append(to);
    command.append("'");
    cout << "Renaming " << from << " to " << to <<"\n";
    adb_shell(command);
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

    string command = "rm -r '";
    command.append(path_string);
    command.append("'");
    adb_shell(command);
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

    string command = "rm '";
    command.append(path_string);
    command.append("'");
    adb_shell(command);
    invalidateCache(path_string);
    unlink(local_path_string.c_str());
    return 0;
}

static int adb_readlink(const char *path, char *buf, size_t size)
{
    string path_string(path);
    string_replacer(path_string,"'","\\'");
    queue<string> output;
    string command = "ls -l \"";
    int num_slashes, ii;
    for (num_slashes = ii = 0; ii < strlen(path); ii++)
	    if (path[ii] == '/')
		    num_slashes++;
    if (num_slashes >= 1) num_slashes--;
    command.append(path_string);
    command.append("\"");
    output = adb_shell(command);
    if(output.empty())
       return -EINVAL;
    string res = output.front();
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

/**
   Main struct for FUSE interface.
 */
static struct fuse_operations adbfs_oper;

/**
   Set up the fuse_operations struct adbfs_oper using above adb_*
   functions and then call fuse_main to manage things.

   @see fuse_main in fuse.h.
 */
int main(int argc, char *argv[])
{
    signal(SIGSEGV, handler);   // install our handler
    makeTmpDir();
    memset(&adbfs_oper, sizeof(adbfs_oper), 0);
    adbfs_oper.readdir= adb_readdir;
    adbfs_oper.getattr= adb_getattr;
    adbfs_oper.access= adb_access;
    adbfs_oper.open= adb_open;
    adbfs_oper.flush = adb_flush;
    adbfs_oper.release = adb_release;
    adbfs_oper.read= adb_read;
    adbfs_oper.write = adb_write;
    adbfs_oper.utimens = adb_utimens;
    adbfs_oper.truncate = adb_truncate;
    adbfs_oper.mknod = adb_mknod;
    adbfs_oper.mkdir = adb_mkdir;
    adbfs_oper.rename = adb_rename;
    adbfs_oper.rmdir = adb_rmdir;
    adbfs_oper.unlink = adb_unlink;
    adbfs_oper.readlink = adb_readlink;
    return fuse_main(argc, argv, &adbfs_oper, NULL);
}
