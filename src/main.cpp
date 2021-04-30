/*
 * Loudness normalizer based on the EBU R128 standard
 *
 * Copyright (c) 2014, Alessandro Ghedini
 * All rights reserved.
 *
 * 2019-06-30 - v0.2.1 - Matthias C. Hormann
 *  - Added version
 *  - Added writing tags to Ogg Vorbis files (now supports MP3, FLAC, Ogg Vorbis)
 *  - Always remove REPLAYGAIN_REFERENCE_LOUDNESS, wrong value might confuse players
 *  - Added notice in help on which file types can be written
 *  - Added album summary
 * 2019-07-07 - v0.2.2 - Matthias C. Hormann
 *  - Fixed album peak calculation.
 *  - Write REPLAYGAIN_ALBUM_* tags only if in album mode
 *  - Better versioning (CMakeLists.txt → config.h)
 *  - TODO: clipping calculation still wrong
 * 2019-07-08 - v0.2.4 - Matthias C. Hormann
 *  - add "-s e" mode, writes extra tags (REPLAYGAIN_REFERENCE_LOUDNESS,
 *    REPLAYGAIN_TRACK_RANGE and REPLAYGAIN_ALBUM_RANGE)
 *  - add "-s l" mode (like "-s e" but uses LU/LUFS instead of dB)
 * 2019-07-08 - v0.2.5 - Matthias C. Hormann
 *  - Clipping warning & prevention (-k) now works correctly, both track & album
 * 2019-07-09 - v0.2.6 - Matthias C. Hormann
 *  - Add "-L" mode to force lowercase tags in MP3/ID3v2.
 * 2019-07-10 - v0.2.7 - Matthias C. Hormann
 *  - Add "-S" mode to strip ID3v1/APEv2 tags from MP3 files.
 *  - Add "-I 3"/"-I 4" modes to select ID3v2 version to write.
 *  - First step to implement a new tab-delimited list format: "-O" mode.
 * 2019-07-13 - v0.2.8 - Matthias C. Hormann
 *  - new -O output format: re-ordered, now shows peak before/after gain applied
 *  - -k now defaults to clipping prevention at -1 dBTP (as EBU recommends)
 *  - New -K: Allows clippping prevention with settable dBTP level,
 *     i.e. -K 0 (old-style) or -K -2 (to compensate for post-processing losses)
 * 2019-08-06 - v0.5.3 - Matthias C. Hormann
 *  - Add support for Opus (.opus) files.
 * 2019-08-16 - v0.6.0 - Matthias C. Hormann
 *  - Rework for new FFmpeg API (get rid of deprecated calls)
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
 * IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include <algorithm>
#include <filesystem>
#include <omp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <set>
#include <map>
#include <iostream>
#include <fstream>
#include <chrono>
#include <thread>

extern "C" {
#include <ebur128.h>
#include <libavcodec/avcodec.h>
#include <libavutil/common.h>
#include <libswresample/swresample.h>
#include <libavformat/avformat.h>
}

#include <config.h>
#include <scan.hpp>
#include <tag.hpp>
#include <loudgain.hpp>

#include <argparse.hpp>
#include <taglib/taglib.h>

namespace fs = std::filesystem;

#define UNUSED(x) (void)x


static void version(void)
{
    /* libebur128 version */
    int ebur128_v_major = 0, ebur128_v_minor = 0, ebur128_v_patch = 0;
    char ebur128_version[15] = "";
    ebur128_get_version(&ebur128_v_major, &ebur128_v_minor, &ebur128_v_patch);
    snprintf(ebur128_version, sizeof(ebur128_version), "%d.%d.%d", ebur128_v_major, ebur128_v_minor, ebur128_v_patch);

    /* libavformat version */
    unsigned lavf_ver = 0;
    char lavf_version[15] = "";
    lavf_ver = avformat_version();
    snprintf(lavf_version, sizeof(lavf_version), "%u.%u.%u", lavf_ver>>16, lavf_ver>>8&0xff, lavf_ver&0xff);

    /* libswresample version */
    unsigned swr_ver = 0;
    char swr_version[15] = "";
    swr_ver = swresample_version();
    snprintf(swr_version, sizeof(swr_version), "%u.%u.%u", swr_ver>>16, swr_ver>>8&0xff, swr_ver&0xff);

    /* taglib version */
    char tlib_version[15] = "";
    snprintf(tlib_version, sizeof(tlib_version), "%d.%d.%d", TAGLIB_MAJOR_VERSION, TAGLIB_MINOR_VERSION, TAGLIB_PATCH_VERSION);

    printf("%s %s - using:\n", PROJECT_NAME, PROJECT_VER);
    printf("  %s %s\n", "libebur128", ebur128_version);
    printf("  %s %s\n", "libavformat", lavf_version);
    printf("  %s %s\n", "libswresample", swr_version);
    printf("  %s %s\n", "taglib", tlib_version);
}

int main(int argc, char *argv[])
{
    /* Define arguments */
    argparse::ArgumentParser parser("Loudgain");

    parser.add_argument("--version", "-v").default_value(false).implicit_value(true)
            .help("Show version number and exit.");

    parser.add_argument("--track", "-t").implicit_value(true)
            .help("Calculate track gain only (default).");

    parser.add_argument("--album", "-a").default_value(false).implicit_value(true)
            .help("Calculate album gain (and track gain).");

    parser.add_argument("--ignore-clipping", "-i").default_value(false).implicit_value(true)
            .help("Ignore clipping warning.");

    parser.add_argument("--prevent-clipping", "-p").default_value(false).implicit_value(true)
            .help("Lower track/album gain to avoid clipping (<= -1 dBTP).");

    parser.add_argument("--max-true-peak-level", "-P").nargs(1)
            .help("Avoid clipping. Max true peak level = n dBTP.");

    parser.add_argument("--pre-gain", "-G").default_value(0.0).nargs(1)
            .action([](const std::string& value) { return std::stod(value); })
            .help("Apply n dB/LU pre-gain value (-5 for -23 LUFS target).");

    parser.add_argument("--tagmode", "-S").nargs(1)
            .help("-S d: Delete ReplayGain tags from files\n"
                  "\t\t\t\t-S i: Write ReplayGain 2.0 tags to files\n"
                  "\t\t\t\t-S e: Like '-S i', plus extra tags (reference, ranges)\n"
                  "\t\t\t\t-S s: Don't write ReplayGain tags (default)");

    parser.add_argument("--lufs", "-u").default_value(false).implicit_value(true)
            .help("Set unit to LUFS. Default is dB.");

    parser.add_argument("--lowercase", "-l").default_value(false).implicit_value(true)
            .help("Force lowercase tags (MP2/MP3/MP4/WMA/WAV/AIFF).\n"
                  "\t\t\t\tThis is non-standard but sometimes needed.");

    parser.add_argument("--striptags", "-s").default_value(false).implicit_value(true)
            .help("Strip tag types other than ID3v2 from MP2/MP3.\n"
                  "\t\t\t\tStrip tag types other than APEv2 from WavPack/APE.");

    parser.add_argument("--id3v2version", "-I").default_value(4).nargs(1)
            .action([](const std::string& value) { return std::stoi(value); })
            .help("Write ID3v2.3 or ID3v2.4 (default) tags to MP2/MP3/WAV/AIFF.");

    parser.add_argument("--multithread", "-M").default_value(0).nargs(1)
            .action([](const std::string& value) { return std::stoi(value); })
            .help("Enable multithreading, n = max number of threads.");

    parser.add_argument("--output-tab", "-o").default_value(false).implicit_value(true)
            .help("Prints tab-delimited list output.");

    parser.add_argument("--output-csv", "-O").nargs(1)
            .help("Writes comma separated values to file.");

    parser.add_argument("--recursive", "-r").default_value(false).implicit_value(true)
            .help("Recursive directory and file scan.");

    parser.add_argument("--extensions", "-E").nargs(1)
            .help("Limit scan to specified extensions.");

    parser.add_argument("--verbosity", "-V").default_value(2).nargs(1)
            .action([](const std::string& value) { return std::stoi(value); })
            .help("Set vebosity level.");

    parser.add_argument("--quiet", "-q").default_value(false).implicit_value(true)
            .help("Don't print scanning status messages. Equal to \"-V 1\".");

    parser.add_argument("FILES").remaining();

    /* Try parsing arguments, exit on error */
    try
    {
        parser.parse_args(argc, argv);
    }
    catch (const std::runtime_error& err)
    {
        std::cerr << err.what() << std::endl << std::endl;
        std::cerr << parser.help().rdbuf() << std::endl;
        exit(EXIT_FAILURE);
    }

    if (parser.get<bool>("--version"))
    {
        version();
        return 0;
    }

    try
    {
        std::vector<std::string> files = parser.get<std::vector<std::string>>("FILES");
    }
    catch (std::logic_error& err)
    {
        UNUSED(err);
        std::cerr << "No files or folders provided!" << std::endl << std::endl;
        std::cerr << parser.help().rdbuf() << std::endl;
        exit(EXIT_FAILURE);
    }

    /* libebur128 version check -- versions before 1.2.4 aren’t recommended */
    int ebur128_v_major = 0, ebur128_v_minor = 0, ebur128_v_patch = 0;
    ebur128_get_version(&ebur128_v_major, &ebur128_v_minor, &ebur128_v_patch);
    if (ebur128_v_major <= 1 && ebur128_v_minor <= 2 && ebur128_v_patch < 4)
        std::cerr << "Old libebur128 version detected. Please update to version 1.2.4 or newer!" << std::endl;

    /* Set stdout float formatting */
    std::cout.setf(std::ios::fixed, std::ios::floatfield); // set fixed floating format
    std::cout.precision(2); // for fixed format, two decimal places

    /* Create loudgain object and set options */
    LoudGain lg;

    lg.setVerbosity(parser.get<int>("--verbosity"));
    if (parser.get<bool>("--quiet"))
        lg.setVerbosity(1);

    lg.setAlbumScanMode(parser.get<bool>("--album"));
    if (parser.present("--tagmode"))
        lg.setTagMode(parser.get<std::string>("--tagmode")[0]);
    else
        lg.setTagMode('s');
    lg.setUnitToLUFS(parser.get<bool>("--lufs"));
    lg.setPregain(parser.get<double>("--pre-gain"));
    lg.setWarnClipping(!parser.get<bool>("--ignore-clipping"));
    lg.setPreventClipping(parser.get<bool>("--prevent-clipping"));
    if (parser.present("--max-true-peak-level"))
        lg.setMaxTruePeakLevel(std::stod(parser.get<std::string>("--max-true-peak-level")));

    lg.setForceLowerCaseTags(parser.get<bool>("--lowercase"));  // force MP3 ID3v2 tags to lowercase
    lg.setStripTags(parser.get<bool>("--striptags"));           // MP3 ID3v2: strip other tag types
    lg.setID3v2Version(parser.get<int>("--id3v2version"));      // MP3 ID3v2 version to write; can be 3 or 4

    lg.setTabOutput(parser.get<bool>("--output-tab"));
    if (bool(parser.present("--output-csv")))
        lg.openCsvFile(parser.get<std::string>("--output-csv"));

    lg.setNumberOfThreads(parser.get<int>("--multithread"));

    auto t1 = std::chrono::high_resolution_clock::now();

    AudioLibrary library;
    std::vector<std::string> files = parser.get<std::vector<std::string>>("FILES");
    library.setLibraryPaths(files);
    library.setRecursive(parser.get<bool>("--recursive"));

    if (bool(parser.present("--extensions")))
        library.setUserExtensions(parser.get<std::string>("--extensions"));

    if (lg.tagMode == 'd')
    {
        if (lg.verbosity > 0)
            std::cout << "Deleting replay gain tags..." << std::endl;
        library.removeReplayGainTags(lg);
    }
    else
    {
        if (lg.verbosity > 0)
            std::cout << "Starting scan..." << std::endl;
        library.scanLibrary(lg);
    }
    lg.closeCsvFile();

    auto t2 = std::chrono::high_resolution_clock::now();

    double duration = std::chrono::duration<double, std::ratio<1,1>>(t2 - t1).count();

    if (lg.verbosity > 0)
    {
        if (duration < 60.0)
            std::cout << "Finished in " << duration << " seconds" << std::endl;
        else
        {
            int du = int(round(duration));
            std::cout << "Finished in " << (du / 60) << "m:" << (du % 60) << "s" << std::endl;
        }
    }

    return 0;
}
