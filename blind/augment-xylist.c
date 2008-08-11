/*
 This file is part of the Astrometry.net suite.
 Copyright 2007-2008 Dustin Lang, Keir Mierle and Sam Roweis.

 The Astrometry.net suite is free software; you can redistribute
 it and/or modify it under the terms of the GNU General Public License
 as published by the Free Software Foundation, version 2.

 The Astrometry.net suite is distributed in the hope that it will be
 useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	See the GNU
 General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with the Astrometry.net suite ; if not, write to the Free Software
 Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA	 02110-1301 USA
 */

/**
 * Accepts an xylist and command-line options, and produces an augmented
 * xylist.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <libgen.h>
#include <getopt.h>

#include "ioutils.h"
#include "bl.h"
#include "an-bool.h"
#include "solver.h"
#include "math.h"
#include "fitsioutils.h"
#include "blindutils.h"
#include "sip_qfits.h"
#include "tabsort.h"
#include "errors.h"
#include "fits-guess-scale.h"
#include "image2xy-files.h"
#include "resort-xylist.h"
#include "qfits.h"
#include "an-opts.h"
#include "augment-xylist.h"
#include "log.h"

void augment_xylist_init(augment_xylist_t* axy) {
    memset(axy, 0, sizeof(augment_xylist_t));
    axy->tempdir = "/tmp";
    axy->tweak = TRUE;
    axy->tweakorder = 2;
    axy->depths = il_new(4);
    axy->fields = il_new(16);
	axy->scales = dl_new(16);
    axy->verifywcs = sl_new(4);
    axy->try_verify = TRUE;
    axy->resort = TRUE;
    axy->ra_center = HUGE_VAL;
    axy->dec_center = HUGE_VAL;
}

void augment_xylist_free_contents(augment_xylist_t* axy) {
    sl_free2(axy->verifywcs);
    dl_free(axy->scales);
    il_free(axy->depths);
    il_free(axy->fields);
}

void augment_xylist_print_special_opts(an_option_t* opt, bl* opts, int index,
                                       FILE* fid, void* extra) {
    if (!strcmp(opt->name, "image")) {
        fprintf(fid, "%s",
                "  (   -i / --image  <image-input-file>\n"
                "   OR -x / --xylist <xylist-input-file>  ): input file\n");
    } else if (!strcmp(opt->name, "scale-units")) {
        fprintf(fid, "%s",
                "  -u / --scale-units <units>: in what units are the lower and upper bound specified?\n"
                "     choices:  \"degwidth\"    : width of the image, in degrees\n"
                "               \"arcminwidth\" : width of the image, in arcminutes\n"
                "               \"arcsecperpix\": arcseconds per pixel\n"
                );
    } else if (!strcmp(opt->name, "depth")) {
        fprintf(fid, "%s",
                "  -d / --depth <number or range>: number of field objects to look at, or range of numbers;\n"
                "                                  1 is the brightest star, so \"-d 10\" or \"-d 1-10\" mean look at\n"
                "                                  the top ten brightest stars only.\n");
    } else if (!strcmp(opt->name, "xylist-only")) {
        fprintf(fid, "%s",
                "The following options are valid for xylist inputs only:\n");
    } else if (!strcmp(opt->name, "fields")) {
        fprintf(fid, "%s",
                "  -F / --fields <number or range>: the FITS extension(s) to solve, inclusive\n");
    } else if (!strcmp(opt->name, "options")) {
        fprintf(fid, "Options include:\n");
    }
}

static an_option_t options[] = {
	{'i', "image",		   required_argument, NULL, NULL},
    {'x', "xylist",		   required_argument, NULL, NULL},
	{'o', "out",		   required_argument, "filename",
     "output augmented xylist filename"},
    {'\x01', "options",       no_argument, NULL, NULL},
	{'h', "help",		   no_argument, NULL,
     "print this help message" },
	{'v', "verbose",       no_argument, NULL,
     "be more chatty" },
	{'L', "scale-low",	   required_argument, "scale",
     "lower bound of image scale estimate"},
    {'H', "scale-high",   required_argument, "scale",
     "upper bound of image scale estimate"},
	{'u', "scale-units",    required_argument, "units", NULL},
    {'3', "ra",             required_argument, "degrees or hh:mm:ss",
     "only search in indexes within 'radius' of the field center given by 'ra' and 'dec'"},
    {'4', "dec",            required_argument, "degrees or [+-]dd:mm:ss",
     "only search in indexes within 'radius' of the field center given by 'ra' and 'dec'"},
    {'5', "radius",         required_argument, "degrees",
     "only search in indexes within 'radius' of the field center given by ('ra', 'dec')"},
	{'d', "depth",		   required_argument, NULL, NULL},
    {'l', "cpulimit",       required_argument, "seconds",
     "give up solving after the specified number of seconds of CPU time"},
    {'r', "resort",         no_argument, NULL,
     "sort the star brightnesses by background-subtracted flux; the default is to sort using a\n"
     "    compromise between background-subtracted and non-background-subtracted flux"},
    {'z', "downsample",     required_argument, "int",
     "downsample the image by factor <int> before running source extraction"},
	{'C', "cancel",		   required_argument, "filename",
     "filename whose creation signals the process to stop"},
	{'S', "solved",		   required_argument, "filename",
     "output file to mark that the solver succeeded"},
	{'I', "solved-in",     required_argument, "filename",
     "input filename for solved file"},
	{'M', "match",		   required_argument, "filename",
     "output filename for match file"},
	{'R', "rdls",		   required_argument, "filename",
     "output filename for RDLS file"},
	{'j', "scamp-ref",	   required_argument, "filename",
     "output filename for SCAMP reference catalog"},
    {'B', "corr",          required_argument, "filename",
     "output filename for correspondences"},
	{'W', "wcs",		   required_argument, "filename",
     "output filename for WCS file"},
	{'P', "pnm",		   required_argument, "filename",
     "save the PNM file as <filename>"},
	{'f', "force-ppm",	   no_argument, NULL,
     "force the PNM file to be a PPM"},
    {'k', "keep-xylist",   required_argument, "filename",
     "save the (unaugmented) xylist to <filename>"},
    {'A', "dont-augment",   no_argument, NULL,
     "quit after writing the unaugmented xylist"},
    {'V', "verify",         required_argument, "filename",
     "try to verify an existing WCS file"},
    {'y', "no-verify",     no_argument, NULL,
     "ignore existing WCS headers in FITS input images"},
	{'g', "guess-scale",   no_argument, NULL,
     "try to guess the image scale from the FITS headers"},
	{'T', "no-tweak",	   no_argument,	NULL,
     "don't fine-tune WCS by computing a SIP polynomial"},
	{'t', "tweak-order",    required_argument, "int",
     "polynomial order of SIP WCS corrections"},
    {'c', "code-tolerance", required_argument, "distance",
     "matching distance for quads, default 0.01"},
    {'E', "pixel-error",    required_argument, "pixels",
     "for verification, size of pixel positional error, default 1"},
	{'m', "temp-dir",       required_argument, "dir",
     "where to put temp files, default /tmp"},
    {'q', "quad-size-min",  required_argument, "fraction",
     "minimum size of quads to try, as a fraction of the smaller image dimension, default 0.1"},
    {'Q', "quad-size-max",  required_argument, "fraction",
     "maximum size of quads to try, as a fraction of the image hypotenuse, default 1.0"},
    // placeholder for printing "The following are for xylist inputs only"
    {'\0', "xylist-only", no_argument, NULL, NULL},
    {'F', "fields",         required_argument, NULL, NULL},
	{'w', "width",		   required_argument, "pixels",
     "specify the field width"},
	{'e', "height",		   required_argument, "pixels", 
     "specify the field height"},
	{'2', "no-fits2fits",   no_argument, NULL,
     "don't sanitize FITS files; assume they're already valid"},
	{'X', "x-column",       required_argument, "column-name",
     "the FITS column containing the X coordinate of the sources"},
	{'Y', "y-column",       required_argument, "column-name",
     "the FITS column containing the Y coordinate of the sources"},
    {'s', "sort-column",    required_argument, "column-name",
     "the FITS column that should be used to sort the sources"},
    {'a', "sort-ascending", no_argument, NULL,
     "sort in ascending order (smallest first); default is descending order"},
};

void augment_xylist_print_help(FILE* fid) {
    bl* opts;
    opts = opts_from_array(options, sizeof(options)/sizeof(an_option_t), NULL);
    opts_print_help(opts, fid, augment_xylist_print_special_opts, NULL);
    bl_free(opts);
}

void augment_xylist_add_options(bl* opts) {
    bl* myopts = opts_from_array(options, sizeof(options)/sizeof(an_option_t), NULL);
    bl_append_list(opts, myopts);
    bl_free(myopts);
}

static int parse_fields_string(il* fields, const char* str);

int augment_xylist_parse_option(char argchar, char* optarg,
                                augment_xylist_t* axy) {
    double d;
    switch (argchar) {
    case '3':
        axy->ra_center = atora(optarg);
        if (axy->ra_center == HUGE_VAL) {
            ERROR("Couldn't understand your RA center argument \"%s\"", optarg);
            return -1;
        }
        break;
    case '4':
        axy->dec_center = atodec(optarg);
        if (axy->dec_center == HUGE_VAL) {
            ERROR("Couldn't understand your Dec center argument \"%s\"", optarg);
            return -1;
        }
        break;
    case '5':
        axy->search_radius = atof(optarg);
        break;
    case 'B':
        axy->corrfn = optarg;
        break;
    case 'y':
        axy->try_verify = FALSE;
        break;
    case 'q':
        d = atof(optarg);
        if (d < 0.0 || d > 1.0) {
            ERROR("quad size fraction (-q / --quad-size-min) must be between 0.0 and 1.0");
            return -1;
        }
        axy->quadsize_min = d;
        break;
    case 'Q':
        d = atof(optarg);
        if (d < 0.0 || d > 1.0) {
            ERROR("quad size fraction (-Q / --quad-size-max) must be between 0.0 and 1.0");
            return -1;
        }
        axy->quadsize_max = d;
        break;
    case 'l':
        axy->cpulimit = atof(optarg);
        break;
    case 'A':
        axy->dont_augment = TRUE;
        break;
    case 'z':
        axy->downsample = atoi(optarg);
        break;
    case 'r':
        axy->resort = FALSE;
        break;
    case 'E':
        axy->pixelerr = atof(optarg);
        break;
    case 'c':
        axy->codetol = atof(optarg);
        break;
    case 'v':
        axy->verbosity++;
        break;
    case 'V':
        sl_append(axy->verifywcs, optarg);
        break;
    case 'I':
        axy->solvedinfn = optarg;
        break;
    case 'k':
        axy->keepxylsfn = optarg;
        break;
    case 's':
        axy->sortcol = optarg;
        break;
    case 'a':
        axy->sort_ascending = TRUE;
        break;
    case 'X':
        axy->xcol = optarg;
        break;
    case 'Y':
        axy->ycol = optarg;
        break;
    case 'm':
        axy->tempdir = optarg;
        break;
    case '2':
        axy->no_fits2fits = TRUE;
        break;
    case 'F':
        if (parse_fields_string(axy->fields, optarg)) {
            ERROR("Failed to parse fields specification \"%s\"", optarg);
            return -1;
        }
        break;
    case 'd':
        if (parse_depth_string(axy->depths, optarg)) {
            ERROR("Failed to parse depth specification: \"%s\"", optarg);
            return -1;
        }
        break;
    case 'o':
        axy->outfn = optarg;
        break;
    case 'i':
        axy->imagefn = optarg;
        break;
    case 'x':
        axy->xylsfn = optarg;
        break;
    case 'L':
        axy->scalelo = atof(optarg);
        break;
    case 'H':
        axy->scalehi = atof(optarg);
        break;
    case 'u':
        axy->scaleunits = optarg;
        break;
    case 'w':
        axy->W = atoi(optarg);
        break;
    case 'e':
        axy->H = atoi(optarg);
        break;
    case 'T':
        axy->tweak = FALSE;
        break;
    case 't':
        axy->tweakorder = atoi(optarg);
        break;
    case 'P':
        axy->pnmfn = optarg;
        break;
    case 'f':
        axy->force_ppm = TRUE;
        break;
    case 'g':
        axy->guess_scale = TRUE;
        break;
    case 'S':
        axy->solvedfn = optarg;
        break;
    case 'C':
        axy->cancelfn = optarg;
        break;
    case 'M':
        axy->matchfn = optarg;
        break;
    case 'R':
        axy->rdlsfn = optarg;
        break;
    case 'j':
        axy->scampfn = optarg;
        break;
    case 'W':
        axy->wcsfn = optarg;
        break;
    default:
        return 1;
    }
    return 0;
}

// run(): ppmtopgm, pnmtofits, fits2fits.py
// backtick(): pnmfile, image2pnm.py

static void append_escape(sl* list, const char* fn) {
    sl_append_nocopy(list, shell_escape(fn));
}
static void append_executable(sl* list, const char* fn, const char* me) {
    char* exec = find_executable(fn, me);
    if (!exec) {
        ERROR("Couldn't find executable \"%s\"", fn);
        exit(-1);
    }
    sl_append_nocopy(list, shell_escape(exec));
    free(exec);
}

static sl* backtick(sl* cmd, bool verbose) {
    char* cmdstr = sl_implode(cmd, " ");
    sl* lines;
    logverb("Running: %s\n", cmdstr);
    if (run_command_get_outputs(cmdstr, &lines, NULL)) {
        ERROR("Failed to run command: %s", cmdstr);
        free(cmdstr);
        exit(-1);
    }
    free(cmdstr);
    sl_remove_all(cmd);
    return lines;
}

static void run(sl* cmd, bool verbose) {
    sl* lines = backtick(cmd, verbose);
    sl_free2(lines);
}

int augment_xylist(augment_xylist_t* axy,
                   const char* me) {
	// tempfiles to delete when we finish
    sl* tempfiles;
    sl* cmd;
    bool verbose = axy->verbosity > 0;
    int i, I;
	bool guessed_scale = FALSE;
    bool dosort = FALSE;
    char* xylsfn;
	qfits_header* hdr = NULL;
    int orig_nheaders;
    bool addwh = TRUE;
    FILE* fout = NULL;
    char *fitsimgfn = NULL;

    cmd = sl_new(16);
    tempfiles = sl_new(4);

	if (axy->imagefn) {
		// if --image is given:
		//	 -run image2pnm.py
		//	 -if it's a FITS image, keep the original (well, sanitized version)
		//	 -otherwise, run ppmtopgm (if necessary) and pnmtofits.
		//	 -run image2xy to generate xylist
		char *uncompressedfn;
		char *sanitizedfn;
		char *pnmfn;				
		sl* lines;
        bool iscompressed;
		char* line;
		char pnmtype;
		int maxval;
		char typestr[256];

        uncompressedfn = create_temp_file("uncompressed", axy->tempdir);
		sanitizedfn = create_temp_file("sanitized", axy->tempdir);
        sl_append_nocopy(tempfiles, uncompressedfn);
        sl_append_nocopy(tempfiles, sanitizedfn);

		if (axy->pnmfn)
			pnmfn = axy->pnmfn;
		else {
            pnmfn = create_temp_file("pnm", axy->tempdir);
            sl_append_nocopy(tempfiles, pnmfn);
        }

        append_executable(cmd, "image2pnm.py", me);
        if (!verbose)
            sl_append(cmd, "--quiet");
        if (axy->no_fits2fits)
            sl_append(cmd, "--no-fits2fits");
        else {
            sl_append(cmd, "--sanitized-fits-outfile");
            append_escape(cmd, sanitizedfn);
        }
        sl_append(cmd, "--infile");
        append_escape(cmd, axy->imagefn);
        sl_append(cmd, "--uncompressed-outfile");
        append_escape(cmd, uncompressedfn);
        sl_append(cmd, "--outfile");
        append_escape(cmd, pnmfn);
        if (axy->force_ppm)
            sl_append(cmd, "--ppm");

        lines = backtick(cmd, verbose);

		axy->isfits = FALSE;
        iscompressed = FALSE;
		for (i=0; i<sl_size(lines); i++) {
            logverb("  %s\n", sl_get(lines, i));
			if (!strcmp("fits", sl_get(lines, i)))
				axy->isfits = TRUE;
			if (!strcmp("compressed", sl_get(lines, i)))
				iscompressed = TRUE;
		}
		sl_free2(lines);

		// Get image W, H, depth.
        sl_append(cmd, "pnmfile");
        append_escape(cmd, pnmfn);

        lines = backtick(cmd, verbose);

		if (sl_size(lines) == 0) {
			ERROR("Got no output from the \"pnmfile\" program.");
			exit(-1);
		}
		line = sl_get(lines, 0);
		// eg	"/tmp/pnm:	 PPM raw, 800 by 510  maxval 255"
		if (strlen(pnmfn) + 1 >= strlen(line)) {
			ERROR("Failed to parse output from pnmfile: \"%s\"", line);
			exit(-1);
		}
		line += strlen(pnmfn) + 1;
		if (sscanf(line, " P%cM %255s %d by %d maxval %d",
				   &pnmtype, typestr, &axy->W, &axy->H, &maxval) != 5) {
			ERROR("Failed to parse output from pnmfile: \"%s\"\n", line);
			exit(-1);
		}
		sl_free2(lines);

		if (axy->isfits) {
            if (axy->no_fits2fits) {
                if (iscompressed)
                    fitsimgfn = uncompressedfn;
                else
                    fitsimgfn = axy->imagefn;
            } else
                fitsimgfn = sanitizedfn;

            if (axy->try_verify) {
                char* errstr;
                sip_t sip;
                bool ok;
                // Try to read WCS header from FITS image; if successful,
                // add it to the list of WCS headers to verify.
                logverb("Looking for a WCS header in FITS input image %s\n", fitsimgfn);

                // FIXME - Right now we just try to read SIP/TAN -
                // obviously this should be more flexible and robust.
                errors_start_logging_to_string();
                memset(&sip, 0, sizeof(sip_t));
                ok = (sip_read_header_file(fitsimgfn, &sip) != NULL);
                errstr = errors_stop_logging_to_string(": ");
                if (ok) {
                    logmsg("Found an existing WCS header, will try to verify it.\n");
                    sl_append(axy->verifywcs, fitsimgfn);
                } else {
                    logverb("Failed to read a SIP or TAN header from FITS image.\n");
                    logverb("  (reason: %s)\n", errstr);
                }
                free(errstr);
            }

		} else {
			fitsimgfn = create_temp_file("fits", axy->tempdir);
            sl_append_nocopy(tempfiles, fitsimgfn);
            
			if (pnmtype == 'P') {
                logverb("Converting PPM image to FITS...\n");

                sl_append(cmd, "ppmtopgm");
                append_escape(cmd, pnmfn);
                sl_append(cmd, "| pnmtofits >");
                append_escape(cmd, fitsimgfn);

                run(cmd, verbose);

			} else {
                logverb("Converting PGM image to FITS...\n");

                sl_append(cmd, "pnmtofits");
                append_escape(cmd, pnmfn);
                sl_append(cmd, ">");
                append_escape(cmd, fitsimgfn);

                run(cmd, verbose);
			}
		}

        if (axy->keep_fitsimg) {
            axy->fitsimgfn = strdup(fitsimgfn);
            sl_remove_string(tempfiles, fitsimgfn);
        }

        logmsg("Extracting sources...\n");
        xylsfn = create_temp_file("xyls", axy->tempdir);
        sl_append_nocopy(tempfiles, xylsfn);

        logverb("Running image2xy: input=%s, output=%s\n", fitsimgfn, xylsfn);

        // we have to delete the temp file because otherwise image2xy is too timid to overwrite it.
        if (unlink(xylsfn)) {
            SYSERROR("Failed to delete temp file %s", xylsfn);
            exit(-1);
        }
        // MAGIC 3: downsample by a factor of 2, up to 3 times.
        if (image2xy_files(fitsimgfn, xylsfn, TRUE, axy->downsample, 3)) {
            ERROR("Source extraction failed");
            exit(-1);
        }

        dosort = TRUE;

	} else {
		// xylist.
		// if --xylist is given:
		//	 -fits2fits.py sanitize
        xylsfn = axy->xylsfn;

        if (axy->sortcol)
            dosort = TRUE;

        if (!axy->no_fits2fits) {
            char* sanexylsfn;

            if (axy->keepxylsfn && !dosort) {
                sanexylsfn = axy->keepxylsfn;
            } else {
                sanexylsfn = create_temp_file("sanexyls", axy->tempdir);
                sl_append_nocopy(tempfiles, sanexylsfn);
            }

            append_executable(cmd, "fits2fits.py", me);
            if (verbose)
                sl_append(cmd, "--verbose");
            append_escape(cmd, xylsfn);
            append_escape(cmd, sanexylsfn);

            run(cmd, verbose);
            xylsfn = sanexylsfn;
        }

	}

    if (axy->guess_scale && (fitsimgfn || !axy->imagefn)) {
        dl* estscales = NULL;
        char* infn = (fitsimgfn ? fitsimgfn : xylsfn);
        fits_guess_scale(infn, NULL, &estscales);
        for (i=0; i<dl_size(estscales); i++) {
            double scale = dl_get(estscales, i);
            logverb("Scale estimate: %g\n", scale);
            dl_append(axy->scales, scale * 0.99);
            dl_append(axy->scales, scale * 1.01);
            guessed_scale = TRUE;
        }
        dl_free(estscales);
    }

    if (dosort) {
        char* sortedxylsfn;
        bool do_tabsort = FALSE;

        if (!axy->sortcol)
            axy->sortcol = "FLUX";

        if (axy->keepxylsfn) {
            sortedxylsfn = axy->keepxylsfn;
        } else {
            sortedxylsfn = create_temp_file("sorted", axy->tempdir);
            sl_append_nocopy(tempfiles, sortedxylsfn);
        }

        if (axy->resort) {
            char* err;
            int rtn;
            errors_start_logging_to_string();
            rtn = resort_xylist(xylsfn, sortedxylsfn, axy->sortcol, NULL, axy->sort_ascending);
            err = errors_stop_logging_to_string(": ");
            if (rtn) {
                logmsg("Sorting brightness using %s and BACKGROUND columns failed; falling back to %s.\n",
                       axy->sortcol, axy->sortcol);
                logverb("Reason: %s\n", err);
                do_tabsort = TRUE;
            }
            free(err);

        } else
            do_tabsort = TRUE;

        if (do_tabsort) {
            logverb("Sorting by brightness: input=%s, output=%s, column=%s.\n",
                    xylsfn, sortedxylsfn, axy->sortcol);
            tabsort(xylsfn, sortedxylsfn, axy->sortcol, !axy->sort_ascending);
        }

		xylsfn = sortedxylsfn;
    }

    if (axy->dont_augment)
        // done!
        goto cleanup;

	// start piling FITS headers in there.
	hdr = qfits_header_read(xylsfn);
	if (!hdr) {
		ERROR("Failed to read FITS header from file %s", xylsfn);
		exit(-1);
	}

	orig_nheaders = qfits_header_n(hdr);

    if (!(axy->W && axy->H)) {
        // Look for existing IMAGEW and IMAGEH in primary header.
        axy->W = qfits_header_getint(hdr, "IMAGEW", 0);
        axy->H = qfits_header_getint(hdr, "IMAGEH", 0);
        if (axy->W && axy->H) {
            addwh = FALSE;
        } else {
            // Look for IMAGEW and IMAGEH headers in first extension, else bail.
            qfits_header* hdr2 = qfits_header_readext(xylsfn, 1);
            axy->W = qfits_header_getint(hdr2, "IMAGEW", 0);
            axy->H = qfits_header_getint(hdr2, "IMAGEH", 0);
            qfits_header_destroy(hdr2);
        }
        if (!(axy->W && axy->H)) {
            ERROR("Error: image width and height must be specified for XYLS inputs");
            exit(-1);
        }
    }

    // we may write long filenames.
    fits_header_add_longstring_boilerplate(hdr);

    if (addwh) {
        fits_header_add_int(hdr, "IMAGEW", axy->W, "image width");
        fits_header_add_int(hdr, "IMAGEH", axy->H, "image height");
    }
	qfits_header_add(hdr, "ANRUN", "T", "Solve this field!", NULL);

    if (axy->cpulimit > 0)
        fits_header_add_double(hdr, "ANCLIM", axy->cpulimit, "CPU time limit (seconds)");

    if (axy->xcol)
        qfits_header_add(hdr, "ANXCOL", axy->xcol, "Name of column containing X coords", NULL);
    if (axy->ycol)
        qfits_header_add(hdr, "ANYCOL", axy->ycol, "Name of column containing Y coords", NULL);

	if ((axy->scalelo > 0.0) || (axy->scalehi > 0.0)) {
		double appu, appl;
		if (!axy->scaleunits || !strcasecmp(axy->scaleunits, "degwidth")) {
			appl = deg2arcsec(axy->scalelo) / (double)axy->W;
			appu = deg2arcsec(axy->scalehi) / (double)axy->W;
		} else if (!strcasecmp(axy->scaleunits, "arcminwidth")) {
			appl = arcmin2arcsec(axy->scalelo) / (double)axy->W;
			appu = arcmin2arcsec(axy->scalehi) / (double)axy->W;
		} else if (!strcasecmp(axy->scaleunits, "arcsecperpix")) {
			appl = axy->scalelo;
			appu = axy->scalehi;
		} else
			exit(-1);

		dl_append(axy->scales, appl);
		dl_append(axy->scales, appu);
	}

    /* Hmm, do we want this??
     if ((dl_size(axy->scales) > 0) && guessed_scale)
     qfits_header_add(hdr, "ANAPPDEF", "T", "try the default scale range too.", NULL);
     */

	for (i=0; i<dl_size(axy->scales)/2; i++) {
		char key[64];
        double lo = dl_get(axy->scales, 2*i);
        double hi = dl_get(axy->scales, 2*i + 1);
        if (lo > 0.0) {
            sprintf(key, "ANAPPL%i", i+1);
            fits_header_add_double(hdr, key, lo, "scale: arcsec/pixel min");
        }
        if (hi > 0.0) {
            sprintf(key, "ANAPPU%i", i+1);
            fits_header_add_double(hdr, key, hi, "scale: arcsec/pixel max");
        }
	}

    if (axy->quadsize_min > 0.0)
        fits_header_add_double(hdr, "ANQSFMIN", axy->quadsize_min, "minimum quad size: fraction");
    if (axy->quadsize_max > 0.0)
        fits_header_add_double(hdr, "ANQSFMAX", axy->quadsize_max, "maximum quad size: fraction");

	qfits_header_add(hdr, "ANTWEAK", (axy->tweak ? "T" : "F"), (axy->tweak ? "Tweak: yes please!" : "Tweak: no, thanks."), NULL);
	if (axy->tweak && axy->tweakorder)
		fits_header_add_int(hdr, "ANTWEAKO", axy->tweakorder, "Tweak order");

	if (axy->solvedfn)
		fits_header_addf_longstring(hdr, "ANSOLVED", "solved output file", "%s", axy->solvedfn);
	if (axy->solvedinfn)
		fits_header_addf_longstring(hdr, "ANSOLVIN", "solved input file", "%s", axy->solvedinfn);
	if (axy->cancelfn)
		fits_header_addf_longstring(hdr, "ANCANCEL", "cancel output file", "%s", axy->cancelfn);
	if (axy->matchfn)
		fits_header_addf_longstring(hdr, "ANMATCH", "match output file", "%s", axy->matchfn);
	if (axy->rdlsfn)
		fits_header_addf_longstring(hdr, "ANRDLS", "ra-dec output file", "%s", axy->rdlsfn);
	if (axy->scampfn)
		fits_header_addf_longstring(hdr, "ANSCAMP", "SCAMP reference catalog output file", "%s", axy->scampfn);
	if (axy->wcsfn)
		fits_header_addf_longstring(hdr, "ANWCS", "WCS header output filename", "%s", axy->wcsfn);
	if (axy->corrfn)
		fits_header_addf_longstring(hdr, "ANCORR", "Correspondences output filename", "%s", axy->corrfn);
    if (axy->codetol > 0.0)
		fits_header_add_double(hdr, "ANCTOL", axy->codetol, "code tolerance");
    if (axy->pixelerr > 0.0)
		fits_header_add_double(hdr, "ANPOSERR", axy->pixelerr, "star pos'n error (pixels)");

    if ((axy->ra_center != HUGE_VAL) &&
        (axy->dec_center != HUGE_VAL) &&
        (axy->search_radius >= 0.0)) {
        fits_header_add_double(hdr, "ANERA", axy->ra_center, "RA center estimate (deg)");
        fits_header_add_double(hdr, "ANEDEC", axy->dec_center, "Dec center estimate (deg)");
        fits_header_add_double(hdr, "ANERAD", axy->search_radius, "Search radius from estimated posn (deg)");
    }

    for (i=0; i<il_size(axy->depths)/2; i++) {
        int depthlo, depthhi;
        char key[64];
        depthlo = il_get(axy->depths, 2*i);
        depthhi = il_get(axy->depths, 2*i + 1);
        sprintf(key, "ANDPL%i", (i+1));
		fits_header_addf(hdr, key, "", "%i", depthlo);
        sprintf(key, "ANDPU%i", (i+1));
		fits_header_addf(hdr, key, "", "%i", depthhi);
    }

    for (i=0; i<il_size(axy->fields)/2; i++) {
        int lo = il_get(axy->fields, 2*i);
        int hi = il_get(axy->fields, 2*i + 1);
        char key[64];
        if (lo == hi) {
            sprintf(key, "ANFD%i", (i+1));
            fits_header_add_int(hdr, key, lo, "field to solve");
        } else {
            sprintf(key, "ANFDL%i", (i+1));
            fits_header_add_int(hdr, key, lo, "field range: low");
            sprintf(key, "ANFDU%i", (i+1));
            fits_header_add_int(hdr, key, hi, "field range: high");
        }
    }

    I = 0;
    for (i=0; i<sl_size(axy->verifywcs); i++) {
        char* fn;
        sip_t sip;
		int j;

        fn = sl_get(axy->verifywcs, i);
        if (!sip_read_header_file(fn, &sip)) {
            ERROR("Failed to parse WCS header from file \"%s\"", fn);
            continue;
        }
        I++;
        {
            tan_t* wcs = &(sip.wcstan);
            // note, this initialization has to happen *after* you read the WCS header :)
            double vals[] = { wcs->crval[0], wcs->crval[1],
                              wcs->crpix[0], wcs->crpix[1],
                              wcs->cd[0][0], wcs->cd[0][1],
                              wcs->cd[1][0], wcs->cd[1][1] };
            char key[64];
            char* keys[] = { "ANW%iPIX1", "ANW%iPIX2", "ANW%iVAL1", "ANW%iVAL2",
                             "ANW%iCD11", "ANW%iCD12", "ANW%iCD21", "ANW%iCD22" };
            int m, n, order;
            for (j = 0; j < 8; j++) {
                sprintf(key, keys[j], I);
                fits_header_add_double(hdr, key, vals[j], "");
            }

            if (sip.a_order) {
                sprintf(key, "ANW%iSAO", I);
                order = sip.a_order;
                fits_header_add_int(hdr, key, order, "SIP forward polynomial order");
                for (m=0; m<=order; m++) {
                    for (n=0; (m+n)<=order; n++) {
                        if (m+n < 1)
                            continue;
                        sprintf(key, "ANW%iA%i%i", I, m, n);
                        fits_header_add_double(hdr, key, sip.a[m][n], "");
                        sprintf(key, "ANW%iB%i%i", I, m, n);
                        fits_header_add_double(hdr, key, sip.b[m][n], "");
                    }
                }
            }
            if (sip.ap_order) {
                order = sip.ap_order;
                sprintf(key, "ANW%iSAPO", I);
                fits_header_add_int(hdr, key, order, "SIP reverse polynomial order");
                for (m=0; m<=order; m++) {
                    for (n=0; (m+n)<=order; n++) {
                        if (m+n < 1)
                            continue;
                        sprintf(key, "ANW%iAP%i%i", I, m, n);
                        fits_header_add_double(hdr, key, sip.ap[m][n], "");
                        sprintf(key, "ANW%iBP%i%i", I, m, n);
                        fits_header_add_double(hdr, key, sip.bp[m][n], "");
                    }
                }
            }
        }
    }

	fout = fopen(axy->outfn, "wb");
	if (!fout) {
		SYSERROR("Failed to open output file %s", axy->outfn);
		exit(-1);
	}

    logverb("Writing headers to file %s\n", axy->outfn);

	if (qfits_header_dump(hdr, fout)) {
		ERROR("Failed to write FITS header");
		exit(-1);
	}
    qfits_header_destroy(hdr);

	// copy blocks from xyls to output.
	{
		FILE* fin;
		int start;
		int nb;
		struct stat st;

        logverb("Copying body of file %s to output %s.\n", xylsfn, axy->outfn);

		start = fits_blocks_needed(orig_nheaders * FITS_LINESZ) * FITS_BLOCK_SIZE;

		if (stat(xylsfn, &st)) {
			SYSERROR("Failed to stat() xyls file \"%s\"", xylsfn);
			exit(-1);
		}
		nb = st.st_size;

		fin = fopen(xylsfn, "rb");
		if (!fin) {
			SYSERROR("Failed to open xyls file \"%s\"", xylsfn);
			exit(-1);
		}

        if (pipe_file_offset(fin, start, nb - start, fout)) {
            ERROR("Failed to copy the data segment of xylist file %s to %s", xylsfn, axy->outfn);
            exit(-1);
        }
		fclose(fin);
	}
    fclose(fout);

 cleanup:
	for (i=0; i<sl_size(tempfiles); i++) {
		char* fn = sl_get(tempfiles, i);
		if (unlink(fn)) {
			SYSERROR("Failed to delete temp file \"%s\"", fn);
		}
	}

    sl_free2(cmd);
    sl_free2(tempfiles);
    return 0;
}

static int parse_fields_string(il* fields, const char* str) {
    // 10,11,20-25,30,40-50
    while (str && *str) {
        unsigned int lo, hi;
        int nread;
        if (sscanf(str, "%u-%u", &lo, &hi) == 2) {
            sscanf(str, "%*u-%*u%n", &nread);
        } else if (sscanf(str, "%u", &lo) == 1) {
            sscanf(str, "%*u%n", &nread);
            hi = lo;
        } else {
            ERROR("Failed to parse fragment: \"%s\"", str);
            return -1;
        }
        if (lo <= 0) {
            ERROR("Field number %i is invalid: must be >= 1.", lo);
            return -1;
        }
        if (lo > hi) {
            ERROR("Field range %i to %i is invalid: max must be >= min!", lo, hi);
            return -1;
        }
        il_append(fields, lo);
        il_append(fields, hi);
        str += nread;
        while ((*str == ',') || isspace(*str))
            str++;
    }
    return 0;
}

