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

#define FUSE_USE_VERSION 26
#include "utils.h"
#include <unistd.h>
#include <algorithm>

#include<stddef.h>
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
queue<string> adb_shell(const string&, bool);
queue<string> shell(const string&);

static const char PERMISSION_ERR_MSG[] = ": Permission denied";
static const char TOUCH_TOYBOX_GNU_DATE_ERR_MSG[] = "touch: bad '@";
static const char TOUCH_BUSYBOX_GNU_DATE_ERR_MSG[] = "touch: invalid date '@";

static char local_tz[64] = "\0";

// GLOBALS determined by initAndroidInfos. Go see the function for more details
// The local Android timezone
static string android_tz = "GMT";
// If the Android cannot handle utimens
static bool disable_utimens = false;
// if coreutils (date/touch) are busybox based
static bool coreutils_have_stat;
// if coreutils (date/touch) are busybox based
static coreutils_type coreutils_touch;
// if we handle tz convertion to utc instead of coreutils (date/touch)
static bool computer_tz_convert = true;
// if we handle tz convertion to local instead of coreutils (date/touch)
static bool computer_tz_convert_local = false;
// if touch supports the GNU extension "@epoch.nanosecs"
static bool touch_gnu_mode = true;
// If we add the nanoseconds at the end of dates
static bool touch_nanoseconds = true;
// The coreutils to force to be used for "touch"
static string forced_coreutils_touch = "";


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
};

static struct fuse_opt adb_opts[] = {
    { "rescan", offsetof(struct adb_config, rescan), true },
    FUSE_OPT_END
};

static struct adb_config adbfs_conf;

#define NO_RECENT_CACHE(FILE) ( fileData.find(FILE) ==  fileData.end() \
    || fileData[FILE].timestamp + 30 < time(NULL) )

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
    actual_command.append("\"");
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
   Convert time_t to the local time given the foreign tz_value

   Based on https://stackoverflow.com/a/71970632 CC BY-SA 4.0 :
   Jonathan Leffler who implemented Ikegami answer

   @param t0 the time to be converted
   @param gnu_format the timezone of the input
 */
static struct tm *time_convert_to_local(time_t t0, char const *tz_value) {
    if (strcmp(tz_value, "GMT") == 0)
        return gmtime(&t0);

    if(local_tz[0] == 0) {
        char old_tz[64] = "-none-";
        char *tz = getenv("TZ");
        if (tz != 0)
            strcpy(old_tz, tz);
    }

    setenv("TZ", tz_value, 1);
    tzset();

    struct tm *lt = localtime(&t0);

    if (strcmp(local_tz, "-none-") == 0)
        unsetenv("TZ");
    else
        setenv("TZ", local_tz, 1);
    tzset();

    return lt;
}

/**
   Convert timespec to touch parameter format

   gnu_format allows to specify the posix epoch directly in touch.
https://www.gnu.org/software/coreutils/manual/html_node/General-date-syntax.html
   Toybox's touch used on Android supports the GNU extension for the
    nanoseconds since 0.8.1 : "@%s.%N" (automatic try)
   Toybox was begin used instead of Toolbox in Android 6.

   @param t the time to be converted
   @param local_time if we pass to touch a UTC "Z" format
 */
static string format_touch_time(
        const struct timespec& t,
        bool local_time = false) {
    ostringstream ss;
    if (coreutils_touch == TOOLBOX_OLD) {
        ss << "-t " << t.tv_sec << '.' << setw(9) << setfill('0') << t.tv_nsec;
        return ss.str();
    }
    if (coreutils_touch == TOOLBOX) {
        char buffer[16]; // 20260101.120101\0
        struct tm *lt = gmtime(&(t.tv_sec));
        strftime(buffer, sizeof(buffer), "%Y%m%d.%H%M%S", lt);
        ss << "-t " << buffer;
        return ss.str();
    }

    ss << "-d ";
    if (coreutils_touch == BUSYBOX) {
        char buffer[16]; // 200001010000.00\0
        struct tm *lt = time_convert_to_local(t.tv_sec, android_tz.c_str());
        strftime(buffer, sizeof(buffer), "%Y%m%d%H%M.%S", lt);
        ss << buffer;
        return ss.str();
    }

    if (touch_gnu_mode) {
        ss << "@" << t.tv_sec;
    } else if (computer_tz_convert) {
        char buffer[20]; // 2000-01-01 00:00:00\0
        struct tm *lt;
        if (local_time || computer_tz_convert_local)
            lt = time_convert_to_local(t.tv_sec, android_tz.c_str());
        else
            lt = gmtime(&(t.tv_sec));
        strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", lt);
        ss << buffer;
    } else {
        if (local_time)
            ss << "`date -d ";
        else
            ss << "`date -ud ";
        ss << "@" << t.tv_sec
           << " +%Y-%m-%dT%H:%M:%S`";
    }
    if (touch_nanoseconds)
        ss << '.' << setw(9) << setfill('0') << t.tv_nsec;
    if (!touch_gnu_mode && (!local_time || (computer_tz_convert && computer_tz_convert_local)))
        ss << "Z";
    return ss.str();
}

/**
   Convert a string from stat or ls to a time_t struct

   @param date string describing the date : 2026-05-08
   @param time string describing the time : 01:16:12.123456781
   @param time_zone string describing the time zone :
        +0200 (optional can be '' to be treated as local time)
 */
static time_t convert_str_to_date(string date, string time, string time_zone) {
    time_t res;

    vector<string> ymd = make_array(date, "-");
    vector<string> hm = make_array(time, ":");

    struct tm ftime_l;
    ftime_l.tm_year = atoi(ymd[0].c_str()) - 1900;
    ftime_l.tm_mon  = atoi(ymd[1].c_str()) - 1;
    ftime_l.tm_mday = atoi(ymd[2].c_str());
    ftime_l.tm_hour = atoi(hm[0].c_str());
    ftime_l.tm_min  = atoi(hm[1].c_str());
    if (hm.size() >= 3) {
        // if we have the seconds or the simple format
        // (available in 'ls -ll' but not in 'ls -l')
        vector<string> sm = make_array(time, ".");
        ftime_l.tm_sec  = atoi(sm[0].c_str());
        // nanoseconds can be in the sm[1] value if supported.
        // However time_t cannot have nanoseconds precision
        // so we don't care here.
    }
    else
        ftime_l.tm_sec = 0;
    ftime_l.tm_isdst = -1;
    res = timegm(&ftime_l);

    const char *android_zone = android_tz.c_str();
    if (! (time_zone.empty()))
        android_zone = time_zone.c_str();
    if (strcmp(android_zone, "GMT") == 0)
        return res;
    struct tm *ftime = time_convert_to_local(res, android_zone);
    res = timegm(ftime);
    return res;
}

void printCoreUtilsName(
        coreutils_type core_type,
        bool toolbox_version_unkown,
        bool busybox_touch_no_date,
        bool busybox_version_unkown,
        bool toybox_touch_bugged) {
    switch (core_type) {
    case TOOLBOX_OLD:
        cout << "TOOLBOX_OLD"; break;
    case TOOLBOX:
        if (toolbox_version_unkown)
            cout << "TOOLBOX (unknown version)";
        else
            cout << "TOOLBOX";
        break;
    case BUSYBOX:
        if (busybox_touch_no_date)
            cout << "BUSYBOX (<1.15.0)";
        else if (busybox_version_unkown)
            cout << "BUSYBOX (<1.15.0)";
        else
            cout << "BUSYBOX (unknown version)";
        break;
    case TOYBOX:
        if (toybox_touch_bugged)
            cout << "TOYBOX (<0.7.2)";
        else
            cout << "TOYBOX (>=0.7.2)";
        break;
    default: //UNKOWN
        cout << "unknown"; break;
    }
}


void initAndroidInfos() {
    cout << "initAndroidInfos" << endl;

    queue<string> output = adb_shell("date +%Z", false);
    if (output.size() == 1 && ! output.front().empty()) {
        android_tz = output.front();
        cout << "Android time zone is : " << android_tz << endl;
    } else {
        computer_tz_convert_local = false;
        std::cerr << "Cannot determine Android time zone. 'date +%Z' "
            << "gave a unxexpected answer :" << endl;
        while (! output.empty()) {
            std::cerr << output.front().c_str() << endl;
            output.pop();
        }
    }

    bool coreutils_is_old = false;

    /*
    Should be true after Android 6 (Marshmallow) with toybox introduction
    Even if another non-default coreutils could have 'stat', it would be
    unexpected to have a coreutils installed without the 'stat' link present.
    */
    output = adb_shell("stat / 2> /dev/null", false);
    coreutils_have_stat = output.size() != 0 && (! output.front().empty());
    if (! coreutils_have_stat) {
        std::cerr << "'stat' command don't exist. Cannot have precise "
            << "information on files latter." << endl;
        coreutils_is_old = true;
    }

    // Begining with the introduction of Torybox in Android 6 (Marshmallow)
    // Toolbox is always kept but stripped from its interesting commands.
    output = adb_shell("toolbox 2> /dev/null", false);
    bool have_toolbox = output.size() != 0 && output.front() == "Toolbox!";
    bool toolbox_have_touch = true;
    bool toolbox_is_old;
    bool toolbox_version_unkown = false;
    if (have_toolbox) {
        output = adb_shell("toolbox touch 2> /dev/null", false);
        if (output.size() != 0 && output.front() == "touch: no such tool")
            toolbox_have_touch = false;
    }

    output = adb_shell("toybox --version 2> /dev/null", false);
    bool have_toybox = output.size() != 0;
    bool toybox_touch_bugged = false;
    if (have_toybox) {
        // "c96e42498c99-android" or 0.7.0-f9a7ae754c27-android
        string toybox_version_str = output.front();
        vector<string> toybox_version = make_array(toybox_version_str, ".");
        // if version does not have a 0.0.0 format (beta version)
        // or is below 0.7.2
        if (toybox_version.size() < 3 ||
            (atoi(toybox_version[0].c_str()) == 0 &&
                (atoi(toybox_version[1].c_str()) < 7
                || (atoi(toybox_version[1].c_str()) == 7
                    && atoi(toybox_version[2].c_str()) < 2)))
        ) {
            /*
            There is a bug in Toybox < 0.7.2 where in touch localtime_r is
            called with a uninitialized tv_sec. So randomly localtime_r returns
            a EOVERFLOW (Value too large for defined data type).
            Touch blame the input, but it is a bug.
            This was fixed in :
            https://codeberg.org/landley/toybox/commit/9f3d8aa80fa4d7216106610b077b6d6e4e6dbed4
            Example :
            $ touch -ad 2024-02-02T03:02:03 myfile
            touch: bad '2024-02-02T03:02:03': Value too large for defined data type

            Toybox was introduced in Android 6 (Marshmallow) and have this bug
            until Android 8.0 (Oreo) The first version fixed in Android is
            toybox 0.7.3-05146348f7d3-android
            */
            toybox_touch_bugged = true;
        }
    }

    output = adb_shell("busybox 2> /dev/null", false);
    bool have_busybox = output.size() != 0
        && output.front().rfind("BusyBox", 0) == 0;
    bool busybox_touch_no_date = false;
    bool busybox_version_unkown = false;
    if (have_busybox) {
        // BusyBox v1.31.1-meefik (2019-11-09 21:37:43 MSK) multi-call binary.
        string busybox_version_str;
        vector<string> busybox_version = make_array(output.front());
        if (busybox_version.size() < 2) {
            busybox_version_str = "0.0.0";
            busybox_version_unkown = true;
        } else {
            busybox_version_str = busybox_version[1];
        }
        busybox_version = make_array(busybox_version_str, ".");
        if (! (busybox_version.size() >= 2 && (
            atoi(busybox_version[0].c_str()) > 1
            || (atoi(busybox_version[0].c_str()) == 1
                && atoi(busybox_version[1].c_str()) >= 15)))
        ) {
            // https://busybox.net/oldnews.html
            // "touch: implement -t TIME"
            busybox_touch_no_date = true;
        }
    }

    // we checked which coreutils is used for touch
    output = adb_shell("touch --help 2> /dev/null", false);
    bool touch_help_empty = output.size() == 0 || output.front().empty();
    if (! touch_help_empty && output.front().rfind("BusyBox", 0) == 0) {
        coreutils_touch = BUSYBOX;
    } else if (touch_help_empty && have_toolbox) {
        output = adb_shell("touch --help", true);
        // Checking for toolbox
        if (output.size() == 0 || output.front().rfind("touch: usage:", 0) != 0)
            coreutils_touch = UNKOWN;
        else if (output.front().find("[-t time_t]") != string::npos) {
            // touch: usage: touch [-alm] [-t time_t] <file>
            toolbox_is_old = true;
            coreutils_touch = TOOLBOX_OLD;
        } else if (output.front().find("[-t YYYYMMDD[.hhmmss]]")
                != string::npos) {
            // touch: usage: touch [-alm] [-t YYYYMMDD[.hhmmss]] <file>
            coreutils_touch = TOOLBOX;
        } else {
            toolbox_version_unkown = true;
                coreutils_touch = UNKOWN;
        }
    } else if (touch_help_empty)
        coreutils_touch = UNKOWN;
    else
        /*
            Else we also suppose its TOYBOX. We have no reason to suppose
            it is otherwise. It is the more POSIX compliant version so even
            if it is not TOYBOX it should work in most cases.
        */
        coreutils_touch = TOYBOX;

    if (coreutils_touch == UNKOWN)
        std::cerr << "Error: 'touch' is not giving any help page. And it is "
            << "not toolbox/busybox/toybox. Hoping for the best by supposing "
            << "it is posix compliant version" << endl;

    cout << "Coreutils found : ";
    if (have_toolbox) {
        if (toolbox_is_old)
            printCoreUtilsName(
                TOOLBOX_OLD,
                toolbox_version_unkown,
                busybox_touch_no_date,
                busybox_version_unkown,
                toybox_touch_bugged);
        else
            printCoreUtilsName(
                TOOLBOX,
                toolbox_version_unkown,
                busybox_touch_no_date,
                busybox_version_unkown,
                toybox_touch_bugged);
        if (have_busybox || have_toybox)
            cout << " ";
    }
    if (have_toybox) {
        printCoreUtilsName(
            TOYBOX,
            toolbox_version_unkown,
            busybox_touch_no_date,
            busybox_version_unkown,
            toybox_touch_bugged);
        if (have_busybox)
            cout << " ";
    }
    if (have_busybox) {
        printCoreUtilsName(
            BUSYBOX,
            toolbox_version_unkown,
            busybox_touch_no_date,
            busybox_version_unkown,
            toybox_touch_bugged);
    }
    cout << endl;

    cout << "Default coreutils was identified as : ";
    printCoreUtilsName(
        coreutils_touch,
        toolbox_version_unkown,
        busybox_touch_no_date,
        busybox_version_unkown,
        toybox_touch_bugged);
    cout << endl;


    /* Order of priority for touch :
        - TOYBOX (>=0.7.2)   => New + epoch set (via gnu extension)
        - TOOLBOX_OLD        => epoch set but old tools
        - TOOLBOX            => works but have to be carefull with timzones
        - BUSYBOX (>=1.15.0) => works but no -a but only -m option
        - UNKOWN             => don't know what will happens
        - BUSYBOX (unknown)  => don't know what will happens and no -a option
        - TOYBOX (<0.7.2)    => bugged (almost never works)
        - BUSYBOX (<1.15.0)  => no touch with date
        - TOOLBOX-notouch    => no touch
    */
    bool coreutils_overwrite = true;
    if (coreutils_touch != TOYBOX && have_toybox && ! toybox_touch_bugged) {
        // TOYBOX (>=0.7.2)
        coreutils_touch = TOYBOX;
        forced_coreutils_touch = "toybox ";
    } else if (coreutils_touch != TOOLBOX_OLD && coreutils_touch != TOOLBOX
            && have_toolbox && toolbox_have_touch && ! toolbox_version_unkown) {
        // TOOLBOX_OLD / TOOLBOX
        forced_coreutils_touch = "toolbox ";
        if (toolbox_is_old)
            coreutils_touch = TOOLBOX_OLD;
        else
            coreutils_touch = TOOLBOX_OLD;
    } else if (coreutils_touch != TOOLBOX_OLD && coreutils_touch != TOOLBOX
            && have_busybox && ! busybox_version_unkown
                && ! busybox_touch_no_date) {
        // BUSYBOX (>=1.15.0) / BUSYBOX (unknown)
        coreutils_touch = BUSYBOX;
        forced_coreutils_touch = "busybox ";
    } else if (coreutils_touch != TOYBOX && have_toybox) {
        // TOYBOX (<0.7.2)
        coreutils_touch = TOYBOX;
        forced_coreutils_touch = "toybox ";
    } else if ((coreutils_touch == BUSYBOX && busybox_touch_no_date)
            || (coreutils_touch == TOOLBOX && ! toolbox_have_touch)) {
        // BUSYBOX (<1.15.0)/TOOLBOX-notouch
        std::cerr << "Didn't find a working 'touch' command. Desactivating "
            << "feature (utimens)..." << endl;
        disable_utimens = true;
    } else
        coreutils_overwrite = false;

    if (coreutils_overwrite) {
        cout << "Found a better alternative coreutils to use touch : ";
        printCoreUtilsName(
            coreutils_touch,
            toolbox_version_unkown,
            busybox_touch_no_date,
            busybox_version_unkown,
            toybox_touch_bugged);
        cout << endl;
    } else
        cout << "Using default coreutils for 'touch'." << endl;

    switch (coreutils_touch) {
    case TOOLBOX_OLD:
        coreutils_is_old = true; break;
    case TOOLBOX:
        if (toolbox_version_unkown)
            std::cerr << "Cannot determine Toolbox version." << endl;
        else if (! toolbox_have_touch) {
            std::cerr << "Toolbox don't have a working 'touch' command."
                << endl;
            disable_utimens = true;
        }
        break;
    case BUSYBOX:
        if (busybox_touch_no_date)
            std::cerr << "Busybox is too old (< 1.15.0) to manage time file "
                << "chanches (touch -d does not exists). Desactivating feature "
                << "(utimens)..." << endl;
        else if (busybox_version_unkown)
            std::cerr << "Busybox was found as the best coreutils but cannot "
                << "determine BusyBox version." << endl;
        break;
    case TOYBOX:
        if (toybox_touch_bugged)
            std::cerr << "Toybox is too old (< 0.7.2) to manage time file "
                << "chanches (touch -d is bugged). Be prepare to not be able "
                << "not change file change/modification times." << endl;
        break;
    default: //UNKOWN
        std::cerr << "Can't detect which coreutils to use. Using the default "
            << "one which is unknown. Hoping for posix compliant commands..."
            << endl;
        break;
    }

    if (coreutils_is_old)
        std::cerr << "You are probably using a old Android version. "
        << "Not all operations are guarantee." << endl;

    touch_gnu_mode = ! (
        coreutils_touch == BUSYBOX
        || coreutils_touch == TOOLBOX_OLD
        || coreutils_touch == TOOLBOX);
    touch_gnu_mode = touch_gnu_mode
        && ! (coreutils_touch == TOYBOX && toybox_touch_bugged);
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
  if (file[0] == '/') return false;
  if (file[1] != 'r' && file[1] != '-') return false;
  return true;
}

/**
   Populate the stat file cache by calling 'stat'

   @param path_string The path to the file to update
 */
static void cache_stat(string path_string) {
    /*
        %D = Device ID (hex) (st_dev)
        %i = inode (ino_t)
        %f = All mode bits (hex) (mode_t)
        %h = number of ahrd links (st_nlink)
        %u = User ID (uid_t)
        %g = Group ID (gid_t)
        %t = dev type (dev_t)
        %s = size (st_size)
        %b = Size/512 (st_blocks)
        %B = Bytes per %b (512) (st_blksize)
        %X = Access unix time (st_atim)
        %Y = Mod unix time (st_mtim)
        %Z = Creation unix time (st_ctim)
        Nanoseconds could be accessed through %x/%y/%z and by using
            convert_str_to_date adapted to nanoseconds.
        However the output strut, stat, don't support nanoseconds.
        caching stat '/system/usr' =
            fd1c 25194110 a1a4 0 0 0 7 8 512 1761534769 1761534769 1761534769
    */
    if (! coreutils_have_stat) {
        fileData[path_string].noStat = false;
        fileData[path_string].statOutput.erase();
        return;
    }

    string command = "stat -c '%D %i %f %h %u %g %t %s %b %B %X %Y %Z' '";
    command.append(path_string);
    command.append("'");
    queue<string> output = adb_shell(command, true);
    cout << "caching stat '" << path_string;
    if (output.empty() || output.front().empty()) {
        cout << "' (failed) = ";
        fileData[path_string].statOutput.erase();
    } else {
        cout << "' = ";
        fileData[path_string].statOutput = output.front();
    }
    fileData[path_string].noStat = false;
    if (output.empty())
        cout << endl;
    else
        cout << output.front() <<  endl;
}

static int adb_getattr(const char *path, struct stat *stbuf)
{
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
    if (NO_RECENT_CACHE(path_string)) {
        // In old Android versions -ll is treated as -l. We handle that later.
        string command = "ls -llad '";
        command.append(path_string);
        command.append("'");
        output = adb_shell(command, true);
        if (output.empty()) return -EAGAIN; /* no phone */
        // error format: "/sbin/healthd: Permission denied"
        cout << "caching ls '" << path_string;
        if (
            output.front().length() > sizeof(PERMISSION_ERR_MSG) &&
            (!output.front().compare(
                output.front().length() - sizeof(PERMISSION_ERR_MSG) + 1,
                sizeof(PERMISSION_ERR_MSG) - 1,
                PERMISSION_ERR_MSG)))
        {
            cout << "' (failed) = ";
            fileData[path_string].lsOutput.erase();
            fileData[path_string].statOutput.erase();
            fileData[path_string].noStat = false;
            cout << output.front() << endl;
        } else {
            cout << "' = ";
            output_chunk = make_array(output.front());
            fileData[path_string].lsOutput = output.front();
            cout << output.front() << endl;
            cache_stat(path_string);
        }

        fileData[path_string].timestamp = time(NULL);
    } else{
        output_chunk = make_array(fileData[path_string].lsOutput);
        cout << "from cache " << path << "\n";
    }
    if (fileData[path_string].lsOutput.empty()) {
        // return empty structure - file exists, but no info available
        stbuf->st_mode = S_IFREG;
        return res;
    }

    if(!is_valid_ls_output(output_chunk[0])) {
        return -ENOENT;
    }

    // Very old toybox put '?' instead of %t. We need to calcultate uid_offset
    // now to fall back to ls on that
    long unsigned int uid_offset = 0;
    nlink_t st_nlink_ls = atoi(output_chunk[1].c_str());
    if (st_nlink_ls > 0) uid_offset = 1;
    else st_nlink_ls = 1;

    bool failed_stat = true;
    // lsOutput was populated but never statOutput via other fuse verbs
    if (fileData[path_string].noStat)
        cache_stat(path_string);
    if (! (fileData[path_string].statOutput.empty())) {
        vector<string> output_chunk_stat = make_array(fileData[path_string].statOutput);
        if (output_chunk_stat.size() == 13) {
            failed_stat = false;
            const char * startPtr;
            char * endPtr;
            /*
            struct def :
            https://pubs.opengroup.org/onlinepubs/007904875/basedefs/sys/stat.h.html
            types def :
            https://pubs.opengroup.org/onlinepubs/9799919799/basedefs/sys_types.h.html

            %D %i %f %h %u %g %t %s %b %B %X %Y %Z
            st_dev st_ino st_mode st_nlink st_uid st_gid st_rdev st_size
                st_blocks st_blksize st_atime st_mtime st_ctime
            example for '/system/usr':
            fd1c 25194110 a1a4 1 0 0 0 7 8 512 1761534769 1761534769 1761534769
            */
            for (uint i = 0; i < 13; i++) {
                startPtr = output_chunk_stat[i].c_str();
                switch(i) {
                    case 0: // aDevice ID (fd1c)
                        stbuf->st_dev = strtoull(startPtr, &endPtr, 16);
                        break;
                    case 1: // ino number (25194110)
                        stbuf->st_ino = strtoull(startPtr, &endPtr, 10);
                        break;
                    case 2: // mode (a1a4)
                        stbuf->st_mode = strtoull(startPtr, &endPtr, 16);
                        break;
                    case 3: // number of hard links (1)
                        stbuf->st_nlink = strtoull(startPtr, &endPtr, 10);
                        break;

                    // Owners IDs
                    case 4: // uid (0=root)
                        stbuf->st_uid = strtoull(startPtr, &endPtr, 10);
                        break;
                    case 5: // guid (0=root)
                        stbuf->st_gid = strtoull(startPtr, &endPtr, 10);
                        break;
                    case 6: // (0=normal file)
                        if (strcmp(startPtr, "?") != 0)
                            stbuf->st_rdev = strtoull(startPtr, &endPtr, 16);
                        else if ((stbuf->st_mode & S_IFMT) == S_IFCHR)
                            stbuf->st_rdev =
                                atoi(output_chunk[uid_offset + 3].c_str()) * 256
                                +
                                atoi(output_chunk[uid_offset + 4].c_str());
                        else
                            stbuf->st_rdev = 0;
                        break;

                    // File sizes
                    case 7: // (7)
                        stbuf->st_size = strtoull(startPtr, &endPtr, 10);
                        break;
                    case 8:  // number of block (is signed) (8)
                        stbuf->st_blocks = strtoll(startPtr, &endPtr, 10);
                        break;
                    case 9: // block size (should be 512) (512)
                        stbuf->st_blksize = strtoull(startPtr, &endPtr, 10);
                        break;

                    // File times
                    case 10: // time of last access  (1761534769)
                        stbuf->st_atime = strtoll(startPtr, &endPtr, 10);
                        break;
                    case 11: // time of last modification (1761534769)
                        stbuf->st_mtime = strtoll(startPtr, &endPtr, 10);
                        break;
                    case 12: // time of last status change (1761534769)
                        stbuf->st_ctime = strtoll(startPtr, &endPtr, 10);
                        break;
                }
                if (endPtr == startPtr) {
                    cout << "Failed parsing stat on " << i << endl;
                    failed_stat = true;
                    fileData[path_string].statOutput.erase();
                    break;
                }
            }
        } else {
            fileData[path_string].statOutput.erase();
        }
    }

    if (! failed_stat)
        return res;

    if (coreutils_have_stat)
        cout << "Getting stat data from ls output fall back for '"
            << path << "'\n";
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

    stbuf->st_nlink = st_nlink_ls;

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

    long unsigned int iDate;

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

    time_t now;
    if (output_chunk.size() >= iDate + 4
            && (!output_chunk.empty())
            && output_chunk[iDate + 2].front() == '+'
            && output_chunk[iDate + 2].back() != '\\')
        now = convert_str_to_date(
            output_chunk[iDate],
            output_chunk[iDate+ 1],
            output_chunk[iDate + 2]);
    else
        now = convert_str_to_date(
            output_chunk[iDate],
            output_chunk[iDate+ 1], "");
    stbuf->st_atime = now;   /* time of last access */
    stbuf->st_mtime = now;   /* time of last modification */
    stbuf->st_ctime = now;   /* time of last status change */
    stbuf->st_ino = 1;      /* inode number, fake. */

    // du calculates sizes based on number of 512b blocks
    stbuf->st_blksize = 512;
    stbuf->st_blocks = (stbuf->st_size + 256) / 512;

    return res;
}


size_t find_nth(int n, const string& substr, const string& corpus) {
    size_t p = 0;
    while (n--) {
        if ((( p = corpus.find_first_of(substr, p) )) == string::npos)
            return string::npos;
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

    shell_escape_path(path_string);

    queue<string> output;
    // In old Android versions -ll is treated as -l. We handle that later.
    string command = "ls -lla '";
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
                    if (fname_l == "." || fname_l == "..")
                        continue;
                    cout << "Adding file: '" << fname_l << "':" << endl;
                    filler(buf, fname_l.c_str(), NULL, 0);
                    const string& path_string_c = path_string
                        + (path_string == "/" ? "" : "/") + fname_l;

                    cout << "caching ls '" << path_string_c << "' (failed) = " << output.front() <<  endl;
                    fileData[path_string_c].lsOutput.erase();
                    fileData[path_string_c].statOutput.erase();
                    fileData[path_string_c].noStat = false;
                    fileData[path_string_c].timestamp = time(NULL);
                }
            } else {
                size_t hourSeparatorStart = output.front().find_first_of(":");
                vector<string> restOfLsOutput = make_array(output.front().substr(hourSeparatorStart));
                size_t nameStart;
                if (ranges::count(restOfLsOutput[0], ':') == 2)
                    /*
                    Start of filename =
                        `ls -lla` time separator + two field
                            (rest of time + timezone)
                    restOfLsOutput =
                        ":14:12.895999998 +0200 filename"
                    restOfLsOutput =
                        ":14:12.895999998 +0200 filename"
                    So we skip the rest of the time, the timezone and spaces
                    */
                    nameStart = hourSeparatorStart
                        + restOfLsOutput[0].size()
                        + restOfLsOutput[1].size() + 2;
                else
                    /*
                    if ls does not supports the '-ll' (like very old Androids)
                    we can detect it via the number of : in the time
                    in case of ls -l and not ls -ll it's =
                        `ls -la` hourSeparatorStart + 4
                    */
                    nameStart = hourSeparatorStart + 4;

                const string& fname_l = output.front().substr(nameStart);
                const string fname_n = fname_l.substr(0, fname_l.find(" -> "));
                cout << "Adding file: '" << fname_n <<"':" << endl;
                filler(buf, fname_n.c_str(), NULL, 0);
                const string path_string_c = path_string
                    + (path_string == "/" ? "" : "/") + fname_n;

                cout << "caching ls '" << path_string_c << "' = "
                    << output.front() <<  endl;
                fileData[path_string_c].lsOutput = output.front();
                bool noStat = NO_RECENT_CACHE(path_string_c);
                fileData[path_string_c].noStat = noStat;
                if (noStat)
                    fileData[path_string_c].statOutput.erase();
                fileData[path_string_c].timestamp = time(NULL);
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
        string command = "ls -lad '";
        command.append(path_string);
        command.append("'");
        cout << command<<"\n";
        output = adb_shell(command);
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

static int adb_utimens(const char *path, const struct timespec ts[2]) {
    if (disable_utimens)
        return -ENOSYS;

    string path_string;
    path_string.assign(path);
    fileData[path_string].timestamp = fileData[path_string].timestamp + 50;

    shell_escape_path(path_string);

    bool set_atime = true;
    bool set_mtime = true;
    struct timespec atime = ts[0];
    struct timespec mtime = ts[1];

    // Handle UTIME_NOW / UTIME_OMIT
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    if (ts[0].tv_nsec == UTIME_NOW) {
        atime = now;
    } else if (ts[0].tv_nsec == UTIME_OMIT) {
        set_atime = false;
    }
    if (ts[1].tv_nsec == UTIME_NOW) {
        mtime = now;
    } else if (ts[1].tv_nsec == UTIME_OMIT) {
        set_mtime = false;
    }

    if (!set_atime && !set_mtime)
        return 0;
    string command = "";
    command.append(forced_coreutils_touch);
    if (coreutils_touch == BUSYBOX) {
        if (!set_mtime)
            return 0;
        // Busybox only supports changing last modification datetime
        command.append("touch -d ");
        command.append(format_touch_time(mtime));
        command.append(" '");
        command.append(path_string);
        command.append("'");
    } else {
        if (set_atime) {
            command.append("touch -a ");
            command.append(format_touch_time(atime));
            command.append(" '");
            command.append(path_string);
            command.append("'");
            if (set_mtime)
                command.append(" && ");
        }
        if (set_mtime) {
            command.append("touch -m ");
            command.append(format_touch_time(mtime));
            command.append(" '");
            command.append(path_string);
            command.append("'");
        }
    }

    cout << command<<"\n";
    queue<string> output;
    output = adb_shell(command, true);
    if (!output.empty()) {
        string front = output.front();
        while (!output.empty()) {
            cout << output.front() << "\n";
            output.pop();
        }
        bool date_failed =
                front.length() > sizeof(TOUCH_TOYBOX_GNU_DATE_ERR_MSG)
                && !front.compare(
                    0,
                    sizeof(TOUCH_TOYBOX_GNU_DATE_ERR_MSG) - 1,
                    TOUCH_TOYBOX_GNU_DATE_ERR_MSG);
        date_failed =
            date_failed
            || (front.length() > sizeof(TOUCH_BUSYBOX_GNU_DATE_ERR_MSG)
                && !front.compare(
                    0,
                    sizeof(TOUCH_BUSYBOX_GNU_DATE_ERR_MSG) - 1,
                    TOUCH_BUSYBOX_GNU_DATE_ERR_MSG));

        if (date_failed) {
            if (!touch_gnu_mode)
                return -ENOSYS;

            cout << "Touch doesn't seems to support GNU dates with "
            << "nanoseconds support. Switching to legacy mode." << endl;
            touch_gnu_mode = false;
            return adb_utimens(path, ts);
        }
    }

    cache_stat(path_string);

    // If we forgot to mount -o rescan then we can remount and touch
    // to trigger the scan.
    if (adbfs_conf.rescan) adb_rescan_file(path_string);

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
    string local_path_string_escaped = local_path_string;
    shell_escape_path(path_string);
    shell_escape_path(local_path_string_escaped);


    queue<string> output;
    cout << "adb_truncate" << endl;
    string command = "ls -l -a -d '";
    command.append(path_string);
    command.append("'");
    cout << command << "\n";
    output = adb_shell(command);
    vector<string> output_chunk = make_array(output.front());
    if (output_chunk[0][0] == '/'){
        adb_pull(path_string,local_path_string_escaped);
    }

    fileTruncated[path_string] = true;

    invalidateCache(path_string);

    cout << "truncate[path="
    << local_path_string << "][size=" << size << "]" << endl;

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

static int adb_rename(const char *from, const char *to) {
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
    long num_slashes;
    long unsigned int ii;
    for (num_slashes = ii = 0; ii < strlen(path); ii++)
        if (path[ii] == '/')
            num_slashes++;
    if (num_slashes >= 1) num_slashes--;

    if (NO_RECENT_CACHE(path_string)) {
        string command = "ls -l -a -d '";
        command.append(path_string);
        command.append("'");
        output = adb_shell(command);
        if (output.empty())
            return -EINVAL;
        // error format: "/sbin/healthd: Permission denied"

        cout << "caching ls '" << path_string;
        if ((output.front().length() > sizeof(PERMISSION_ERR_MSG)) &&
           (!output.front().compare(output.front().length() - sizeof(PERMISSION_ERR_MSG) + 1,
                                    sizeof(PERMISSION_ERR_MSG) - 1, PERMISSION_ERR_MSG)))
        {
            cout << "' (failed) = ";
            fileData[path_string].lsOutput.erase();
            fileData[path_string].statOutput.erase();
            fileData[path_string].noStat = false;
        } else {
            cout << "' = ";
            fileData[path_string].lsOutput = output.front();
            bool noStat = NO_RECENT_CACHE(path_string);
            fileData[path_string].noStat = noStat;
            if (noStat)
                fileData[path_string].statOutput.erase();
        }
        cout << output.front() <<  endl;
        fileData[path_string].timestamp = time(NULL);
    } else{
        cout << "from cache " << path << "\n";
    }
    string &res = fileData[path_string].lsOutput;
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
    memset(&adbfs_oper, 0, sizeof(adbfs_oper));
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
    adb_shell("ls");
    initAndroidInfos();

    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
    memset(&adbfs_conf, 0, sizeof(adbfs_conf));
    fuse_opt_parse(&args, &adbfs_conf, adb_opts, NULL);

    return fuse_main(args.argc, args.argv, &adbfs_oper, NULL);
}
