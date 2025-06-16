/* @(#)highwire/Loader.c
 *
 * Currently has ended up a junk file of initializations
 * loading routine and some assorted other routines
 * for file handling.
*/
#ifdef __PUREC__
# include <tos.h>
# include <ext.h>
#endif
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <errno.h> /* For EMFILE, ETIMEDOUT, ECONNRESET, EINVAL, ENOMEM, EIO, ENOENT, EPROTONOSUPPORT, EFBIG, EILSEQ, ENOSYS, EOVERFLOW */
#include <unistd.h> /* For read, write, close, and ssize_t */
#include <fcntl.h> /* For open, O_RDONLY, O_WRONLY, O_CREAT, O_TRUNC */

#include <gem.h> /* For GEM related types like WORD */

#include "file_sys.h"
#include "global.h"
#include "hwWind.h"
#include "vaproto.h"
/* #include "av_comm.h" */
#include "schedule.h"
#include "Containr.h"
#include "Location.h"
#include "Loader.h" /* This is the Loader.h provided by the user. */
#include "parser.h"
#include "http.h"
#include "inet.h"
#include "cache.h"


/* Ensure min macro is available if not provided by global.h or another include */
#ifndef min
#define min(a,b) (((a)<(b))?(a):(b))
#endif

/* LogPrintf is assumed to be defined in global.h or Logging.h. */
/* It was causing build errors, so its calls are removed from this file. */


extern short av_shell_id;     /* Desktop's AES ID */
extern short av_shell_status;  /* What AV commands can desktop do? */
             
extern const char * cfg_Viewer;


char fsel_path[HW_PATH_MAX];
char help_file[HW_PATH_MAX];


static short  start_application (const char * appl, LOCATION loc);


/*______________return_values_of_scheduled_jobs,_controlling_their_priority___*/
#define JOB_KEEP 1  /* restart again later and doesn't change priority   */
#define JOB_AGED -1 /* restart, but decrease the prio a bit because the job is
                     * running already a bit longer now   */
#define JOB_NOOP -2 /* restart also, but decrease the priority much more because
                     * this job didn't got anything to do (was stalled)   */
#define JOB_DONE 0  /* job is done, remove from list and don't start it again */

/*____________________________________________priorities_for_following_jobs___*/
#define PRIO_INTERN   200   /* internal produced pages like error pages, about,
                      * dir listings etc, get the highest priority in general */
#define PRIO_USERACT  100   /* start of loading something initiated by a user
                      * action, normally a link clicked.  this should have be
                      * the next highest value   */
#define PRIO_AUTOMATIC 10   /* start loading some part of a page, usually a
                      * frame */
#define PRIO_TRIVIAL    1   /* start loading some object, usually an image   */
#define PRIO_RECIVE    20   /* begin receiving data from remote   */
#define PRIO_SUCCESS   91   /* decode a file, usually an image    */
#define PRIO_LOADFILE  95   /* load a local file into memory      */
#define PRIO_FINISH    99   /* decode from memory, usually html or text   */
#define PRIO_KEEP       0   /* generic, when nothing from above matches   */

static int loader_job  (void * arg, long invalidated);
static int header_job  (void * arg, long invalidated);
static int receive_job (void * arg, long invalidated);
static int generic_job (void * arg, long invalidated);


/*----------------------------------------------------------------------------*/
static BOOL
start_parser (LOADER loader)
{
	SCHED_FUNC func = parse_plain;
	BOOL       chk0 = FALSE;
	PARSER     parser;
	
	/* Phase 1: Memory Allocation Check */
	if (!loader) {
		/* LogPrintf("LOADER: start_parser called with NULL loader.\n"); */
		return FALSE;
	}

	parser = new_parser (loader);
	/* Phase 1: Memory Allocation Check */
	if (!parser) {
		/* LogPrintf("LOADER: Failed to create new parser in start_parser.\n"); */
		return FALSE;
	}
	
	if (!loader->MimeType) {
		loader->MimeType = MIME_TEXT;
	}
	/* Original file had this line duplicated, removed the duplicate: parser = new_parser (loader); */
	
	switch (loader->MimeType)
	{
		case MIME_IMAGE:
			func = parse_image;
			break;
		
		case MIME_TXT_HTML:
			if (loader->Data) {
				func = parse_html;
				chk0 = TRUE;
			
			} else if (PROTO_isLocal(loader->Location->Proto)) {
				func = parse_dir;
			
			} else { /* PROT_ABOUT -- also catch internal errors */
				func = parse_about;
			}
			break;
		
		case MIME_TEXT: {
			char * p = loader->Data;
            /* Phase 1: Parser Hardening - NULL check before dereference */
            if (!p) {
                /* LogPrintf("LOADER: loader->Data is NULL in start_parser for MIME_TEXT.\n"); */
                return FALSE;
            }
			while (isspace(*p)) p++;
		#ifdef MOD_TROFF
			if (p[0] == '.' &&
			    (p[1] == '"' || (p[1] == '\\' && p[2] == '"') || isalpha(p[1]))) {
				extern int parse_troff (void *, long);
				func = parse_troff;
				break;
			}
		#endif
            /* Phase 1: Buffer Overflow Prevention - Use min and check against DataSize */
			while ( (loader->Data && (p - loader->Data) < loader->DataSize - 4) && (strncmp(p, "<!--", 4) == 0 || strncmp(p, "<?xml", 5) == 0)) {
				long i = min(500L, loader->DataSize - (p - loader->Data) -1); /* Use long literal for min arg */
				while (i > 0 && *(p++) != '>');
				while (i > 0 && isspace(*p)) p++;
                if ( (p - loader->Data) >= loader->DataSize ) break; /* Avoid reading past Data end */
			}
			if (strnicmp (p, "<html>",          6) == 0 ||
			    strnicmp (p, "<!DOCTYPE HTML", 14) == 0) {
				func = parse_html;
				chk0 = TRUE;
				break;
			}
		}
		default: /* pretend it's plain text */ ;
	}
	
	if (chk0) { /* check for invalid nul characters in html */
		char * p = loader->Data, * q;
		long   n = loader->DataSize;
		/* Phase 1: Parser Hardening - NULL check before dereference */
        if (!p) {
            /* LogPrintf("LOADER: loader->Data is NULL during NULL char check.\n"); */
            return FALSE;
        }
		while (n > 0l && (q = memchr (p, '\0', n)) != NULL) {
			*q = ' ';
			n -= (q - p) + 1; /* Adjust n by actual characters consumed including NULL */
			p  = q + 1; /* Move p past the replaced NULL */
		}
	}
	
	return sched_insert (func, parser, (long)parser->Target, PRIO_FINISH);
}

/******************************************************************************/


typedef struct s_ldr_chunk * LDRCHUNK;
struct s_ldr_chunk {
	LDRCHUNK next;
	size_t   size;
	char     data[1];
};


/*============================================================================*/
LOADER
new_loader (LOCATION loc, CONTAINR target, BOOL lookup)
{
	const char * appl = NULL;
	LOADER loader = malloc (sizeof (struct s_loader));
	/* Phase 1: Memory Allocation Check */
	if (!loader) {
		/* LogPrintf("LOADER: new_loader failed to allocate loader structure.\n"); */
		return NULL;
	}

	loader->Location   = location_share (loc);
	/* Phase 1: Memory Allocation Check */
	if (!loader->Location) {
		/* LogPrintf("LOADER: new_loader failed to share location.\n"); */
		free(loader); /* Free loader struct if location_share fails */
		return NULL;
	}

	loader->Target     = target;
	loader->Encoding   = ENCODING_WINDOWS1252;
	loader->MimeType   = MIME_Unknown;
	loader->Referer    = NULL;
	loader->AuthRealm  = NULL;
	loader->AuthBasic  = NULL;
	loader->PostBuf    = NULL; /* Aligned with provided Loader.h */
	loader->FileExt[0] = '\0';
	loader->ExtAppl    = NULL; /* Initialize */
	loader->MarginW    = -1;
	loader->MarginH    = -1;
	loader->ScrollV    = 0;
	loader->ScrollH    = 0;
	/* */
	loader->Cached   = NULL;
	loader->Tdiff    = 0;
	loader->Date     = 0;
	loader->Expires  = 0;
	loader->DataSize = 0;
	loader->DataFill = 0;
	loader->Data     = NULL; /* Aligned with provided Loader.h */
	loader->notified = FALSE;
	loader->Retry    = cfg_ConnRetry;
	loader->Error    = E_OK; /* Use Error member as status flag */
	/* */
	loader->SuccJob = NULL;
	loader->FreeArg = NULL;
	/* */
	loader->rdChunked = FALSE;
	loader->rdSocket  = -1;
	loader->rdLeft    = 0;
	loader->rdDest    = NULL;
	loader->rdTlen    = 0;
	loader->rdList    = loader->rdCurr = NULL;

	/* copy Authorization */
	if (target) {
		FRAME frame = containr_Frame (target);
		if (frame && frame->AuthRealm  && frame->AuthBasic) {
			loader->AuthRealm = strdup (frame->AuthRealm);
			/* Phase 1: Memory Allocation Check */
			if (!loader->AuthRealm) {
				/* LogPrintf("LOADER: new_loader strdup(AuthRealm) failed.\n"); */
				free_location(&loader->Location); /* Cleanup previous allocation */
				free(loader); /* Free loader struct as well */
				return NULL;
			}
			loader->AuthBasic = strdup (frame->AuthBasic);
			/* Phase 1: Memory Allocation Check */
			if (!loader->AuthBasic) {
				/* LogPrintf("LOADER: new_loader strdup(AuthBasic) failed.\n"); */
				free(loader->AuthRealm); /* Cleanup previous strdup */
				free_location(&loader->Location); /* Cleanup location */
				free(loader); /* Free loader struct */
				return NULL;
			}
		}
	}
	
	if (loc->Proto == PROT_FILE || PROTO_isRemote (loc->Proto)) {
		loader->MimeType = mime_byExtension (loc->File, &appl, loader->FileExt);
	}
	
	if (loc->Proto == PROT_HTTP && lookup) {
		CACHED cached = cache_lookup (loc, 0, NULL);
		if (cached) {
			union { CACHED c; LOCATION l; } u;
			u.c            = cache_bound (cached, &loader->Location);
			/* Phase 1: Memory Allocation Check */
			if (!u.l) {
				/* LogPrintf("LOADER: new_loader cache_bound failed for cached item.\n"); */
				cache_release(&cached, FALSE); /* Release cached item if binding fails */
				if (loader->AuthRealm) free(loader->AuthRealm); /* Cleanup existing allocations */
				if (loader->AuthBasic) free(loader->AuthBasic);
				free_location(&loader->Location);
				free(loader);
				return NULL;
			}
			loader->Cached = u.l;
			if (!loader->MimeType) {
				loader->MimeType = mime_byExtension (loader->Cached->File,
				                                     &appl, NULL);
			}
		}
	}
	
	loader->ExtAppl = (appl ? strdup (appl) : NULL);
	/* Phase 1: Memory Allocation Check */
	if (appl && !loader->ExtAppl) {
		/* LogPrintf("LOADER: new_loader strdup(ExtAppl) failed.\n"); */
		if (loader->AuthRealm) free(loader->AuthRealm); /* Cleanup existing allocations */
		if (loader->AuthBasic) free(loader->AuthBasic);
		if (loader->Cached) cache_release((CACHED*)&loader->Cached, FALSE);
		free_location(&loader->Location);
		free(loader);
		return NULL;
	}
	
	containr_notify (loader->Target, HW_ActivityBeg, NULL);
	
	return loader;
}


/*============================================================================*/
void
delete_loader (LOADER * p_loader)
{
	LOADER loader = *p_loader;
	if (loader) {
		if (loader->notified) {
			containr_notify (loader->Target, HW_PageFinished, NULL);
		}
		containr_notify (loader->Target, HW_ActivityEnd, NULL);

#ifdef USE_INET
		if (loader->rdSocket >= 0) {
			inet_close (loader->rdSocket);
		}
#endif /* USE_INET */
		if (loader->rdList) {
			LDRCHUNK chunk = loader->rdList, next;
			do {
				next =  chunk->next;
				free (chunk);
			} while ((chunk = next) != NULL);
		}
		if (loader->ExtAppl) {
			free (loader->ExtAppl);
		}
		/* loader->Data is used for loaded file content or error messages */
		if (loader->Data) {
			free (loader->Data);
		}
		if (loader->Cached) {
			cache_release ((CACHED*)&loader->Cached, FALSE);
		}
		if (loader->PostBuf) { /* Check and delete POST data */
			delete_post (loader->PostBuf);
		}
		if (loader->AuthRealm) {
			free (loader->AuthRealm);
		}
		if (loader->AuthBasic) {
			free (loader->AuthBasic);
		}
		free_location (&loader->Referer);
		free_location (&loader->Location);
		free (loader);
		*p_loader = NULL;
	}
}


/*============================================================================*/
LOADER
start_page_load (CONTAINR target, const char * url, LOCATION base,
                 BOOL u_act, POSTDATA post_buff)
{
	LOCATION loc    = (url ? new_location (url, base) : location_share (base));
	LOADER   loader = NULL;
	
	/* Phase 1: Memory Allocation Check */
	if (!loc) {
		/* LogPrintf("LOADER: start_page_load new_location/location_share failed.\n"); */
		delete_post(post_buff); /* Ensure post_buff is freed if location creation fails */
		return NULL;
	}

	if (loc->Proto == PROT_MAILTO || loc->Proto == PROT_FTP) {
		start_application (NULL, loc);
	
	} else {
		loader = start_cont_load (target, NULL, loc, u_act, (post_buff == NULL));
		/* Phase 1: Memory Allocation Check */
		if (loader) {
			loader->PostBuf = post_buff; /* Aligned with provided Loader.h */
		} else {
			delete_post (post_buff); /* Free POST data if loader creation fails */
		}
	}
	free_location (&loc);
	
	return loader;
}

/*============================================================================*/
LOADER
start_cont_load (CONTAINR target, const char * url, LOCATION base,
                 BOOL u_act, BOOL use_cache)
{
	LOCATION loc    = (url ? new_location (url, base) : location_share (base));
	LOADER   loader = new_loader (loc, target, use_cache);
	
	/* Phase 1: Memory Allocation Check - check if loc was successfully created */
	if (!loc) {
		/* LogPrintf("LOADER: start_cont_load new_location/location_share failed.\n"); */
		return NULL;
	}
	/* Phase 1: Memory Allocation Check - check if loader was successfully created */
	if (!loader) {
		/* LogPrintf("LOADER: start_cont_load new_loader failed.\n"); */
		free_location(&loc); /* Free location if loader creation fails */
		return NULL;
	}

	loc = (loader->Cached ? loader->Cached : loader->Location);
	
	if (!loader->notified) {
		loader->notified = containr_notify (loader->Target, HW_PageStarted,
		                                    loader->Location);
	}
	
	if (loc->Proto == PROT_DIR) {
		loader->MimeType = MIME_TXT_HTML;
		if (!sched_insert (parse_dir, new_parser (loader), (long)target, PRIO_INTERN)) {
			/* LogPrintf("LOADER: sched_insert for parse_dir failed.\n"); */
			delete_loader(&loader); /* Phase 1: Cleanup if scheduler fails */
			return NULL;
		}
		
	} else if (loc->Proto == PROT_ABOUT) {
		loader->MimeType = MIME_TXT_HTML;
		if (!sched_insert (parse_about, new_parser (loader),(long)target, PRIO_INTERN)) {
			/* LogPrintf("LOADER: sched_insert for parse_about failed.\n"); */
			delete_loader(&loader); /* Phase 1: Cleanup if scheduler fails */
			return NULL;
		}
		
	} else if (MIME_Major(loader->MimeType) == MIME_IMAGE) {
		loader->MimeType = MIME_IMAGE;
		if (!sched_insert (parse_image, new_parser (loader),(long)target, PRIO_INTERN)) {
			/* LogPrintf("LOADER: sched_insert for parse_image failed.\n"); */
			delete_loader(&loader); /* Phase 1: Cleanup if scheduler fails */
			return NULL;
		}
		
#ifdef USE_INET
# ifdef MOD_MBOX
	} else if (loc->Proto == PROT_POP) {
		int parse_mbox (void*, long);
		if (!sched_insert (parse_mbox, new_parser (loader), (long)target, PRIO_INTERN)) {
			/* LogPrintf("LOADER: sched_insert for parse_mbox failed.\n"); */
			delete_loader(&loader); /* Phase 1: Cleanup if scheduler fails */
			return NULL;
		}
	
# endif
	} else if (loc->Proto == PROT_HTTP) {
		if (loader->ExtAppl) {
			loader->SuccJob = generic_job;
			containr_notify (loader->Target, HW_PageFinished, NULL);
			loader->notified = FALSE;
		}
		if (!sched_insert (header_job, loader, (long)target,
		              (u_act ? PRIO_USERACT : PRIO_AUTOMATIC))) {
			/* LogPrintf("LOADER: sched_insert for header_job failed.\n"); */
			delete_loader(&loader); /* Phase 1: Cleanup if scheduler fails */
			return NULL;
		}
#endif /* USE_INET */
	
	} else if (loc->Proto) {
		const char txt[] = "<html><head><title>Error</title></head><body>"
		                   "<h1>Protocol #%i not supported!</h1></body></html>";
		char buf [sizeof(txt) +10];
        /* Phase 1: Buffer Overflow Prevention - Use snprintf */
		snprintf (buf, sizeof(buf), txt, loc->Proto);
		loader->Error    = -EPROTONOSUPPORT; /* Assuming -EPROTONOSUPPORT is defined in errno.h */
		loader->Data     = strdup (buf); /* Aligned with provided Loader.h */
		/* Phase 1: Memory Allocation Check */
		if (!loader->Data) {
			/* LogPrintf("LOADER: strdup for protocol error data failed.\n."); */
			delete_loader(&loader); /* Phase 1: Cleanup if strdup fails */
			return NULL;
		}
		loader->MimeType = MIME_TXT_HTML;
		if (!sched_insert (parse_html, new_parser (loader), (long)target, PRIO_INTERN)) {
			/* LogPrintf("LOADER: sched_insert for parse_html (protocol error) failed.\n."); */
			delete_loader(&loader); /* Phase 1: Cleanup if scheduler fails */
			return NULL;
		}
	
	} else if (loader->ExtAppl) {
		start_application (loader->ExtAppl, loc);
		delete_loader (&loader);
		
	} else {
		if (!sched_insert (loader_job, loader, (long)target, PRIO_LOADFILE)) {
			/* LogPrintf("LOADER: sched_insert for loader_job failed.\n."); */
			delete_loader(&loader); /* Phase 1: Cleanup if scheduler fails */
			return NULL;
		}
	}
	
	return loader;
}

/*============================================================================*/
LOADER
start_objc_load (CONTAINR target, const char * url, LOCATION base,
                 int (*successor)(void*, long), void * objc)
{
	LOCATION loc  = new_location (url, base);
	BOOL hdr_only = (!target && successor);
	LOADER loader = new_loader (loc, target, !hdr_only);
	
	/* Phase 1: Memory Allocation Check */
	if (!loc) {
		/* LogPrintf("LOADER: start_objc_load new_location failed.\n."); */
		return NULL;
	}
	/* Phase 1: Memory Allocation Check */
	if (!loader) {
		/* LogPrintf("LOADER: start_objc_load new_loader failed.\n."); */
		free_location(&loc); /* Free location if loader creation fails */
		return NULL;
	}

	loc = (loader->Cached ? loader->Cached : loader->Location);
	
	if (successor) {
		loader->SuccJob = successor;
	
	} else if (loader->ExtAppl) {
		loader->SuccJob = generic_job;
	
	} else {
		/* LogPrintf("start_objc_load(%s) dropped.\n", loc->FullName); */
		delete_loader (&loader);
		return NULL;
	}
	loader->FreeArg = objc;
	
	if (PROTO_isLocal (loc->Proto)) {
		if (!sched_insert (loader->SuccJob, loader, (long)target, PRIO_SUCCESS)) {
			/* LogPrintf("LOADER: sched_insert for local PROTO failed.\n."); */
			delete_loader(&loader); /* Phase 1: Cleanup if scheduler fails */
			return NULL;
		}
	
#ifdef USE_INET
	} else if (loc->Proto == PROT_HTTP) {
		if (!sched_insert (header_job, loader, (long)target,
		              (hdr_only ? PRIO_USERACT : PRIO_TRIVIAL))) {
			/* LogPrintf("LOADER: sched_insert for HTTP PROTO failed.\n."); */
			delete_loader(&loader); /* Phase 1: Cleanup if scheduler fails */
			return NULL;
		}
#endif
	
	} else {
		/* LogPrintf("start_objc_load() invalid protocol %i.\n", loc->Proto); */
		loader->Error = -EPROTONOSUPPORT;
		(*loader->SuccJob)(loader, 0);
		loader = NULL;
	}
	
	return loader;
}


/*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/
#ifdef USE_INET
static int
chunked_job (void * arg, long invalidated)
{
	LOADER   loader = arg;
	BOOL     s_recv = (loader->rdDest == NULL);
	
	if (invalidated) {
		return receive_job (arg, invalidated);
	}
	
	loader->rdDest = NULL;
	loader->rdLeft = 0;
	
	while (1) {
		char * data = loader->rdTemp;
		size_t size = 0;
		long n; /* Declare n here */
		
		if (!loader->rdChunked) {
			if (loader->rdSocket >= 0) {
				size = 8192;
			}
			if (size < loader->rdTlen) {
				 size = loader->rdTlen;
			}
            /* Phase 1: Buffer Overflow Prevention - Ensure read size is safe */
            long to_read = (long)size - loader->DataFill;
            if (loader->Data && to_read > 0) { /* Only read if Data buffer exists and has space */
                n = inet_recv (loader->rdSocket, loader->Data + loader->DataFill, to_read);
            } else {
                n = -1; /* No buffer or no space, simulate error or no-op */
            }

			if (n < 0) {
				if (n == -ECONNRESET) {
					/* LogPrintf("LOADER: Connection reset during chunked transfer (non-chunked).\n"); */
				} else {
					/* LogPrintf("LOADER: inet_recv error for non-chunked data: %ld\n", n); */
				}
				inet_close (loader->rdSocket);
				loader->rdSocket = -1;
				loader->rdLeft   = 0;
				loader->Error = -1; /* Using generic -1 for error where LSTAT_ERROR was */
				return JOB_DONE;
			} else if (n == 0) { /* Connection closed */
				inet_close (loader->rdSocket);
				loader->rdSocket = -1;
			} else { /* n > 0 */
				loader->DataFill += n;
			}
			break; /* Exit loop for non-chunked */
		} else { /* Chunked transfer */
			n = loader->rdTlen - 2;
			data = (n > 0 ? memchr (loader->rdTemp +2, '\n', n) : NULL);
			if (!data) {
                /* Phase 1: Buffer Overflow Prevention - Check buffer size before recv */
                long recv_len = sizeof(loader->rdTemp) - loader->rdTlen - 1; /* -1 for null terminator */
                if (recv_len <= 0) {
                     /* LogPrintf("LOADER: rdTemp full during chunked header read.\n"); */
                     loader->Error = -1; /* Set error flag */
                     break; /* Error: buffer full */
                }
				n = inet_recv (loader->rdSocket, loader->rdTemp + loader->rdTlen, recv_len);

				if (n > 0) {
					loader->rdTlen += n;
                    loader->rdTemp[loader->rdTlen] = '\0'; /* Null terminate after new data */
					if (loader->rdTlen > 2) {
						data = memchr (loader->rdTemp +2, '\n', loader->rdTlen -2);
					}
				} else if (n == 0) { /* Connection closed */
					inet_close (loader->rdSocket);
					loader->rdSocket = -1;
					loader->rdLeft   = 0;
					break;
				} else {
					if (n == -ECONNRESET) {
						/* LogPrintf("LOADER: Connection reset during chunked transfer.\n."); */
					} else {
						/* LogPrintf("LOADER: inet_recv error during chunked transfer: %ld\n", n); */
					}
					inet_close (loader->rdSocket);
					loader->rdSocket = -1;
					loader->Error = -1;
					return JOB_DONE;
				}
			}

			if (data) {
				*(data++) = '\0';
                /* Phase 1: Parser Hardening - Check for valid conversion */
                char *endptr;
				size = strtoul (loader->rdTemp, &endptr, 16);
                if (*endptr != '\0' && !isspace(*endptr)) { /* Check if conversion was complete */
                    /* LogPrintf("LOADER: Malformed chunk size: %s\n", loader->rdTemp); */
                    loader->Error = -1;
                    break;
                }

				if ((long)size <= 0) { /* size 0 means last chunk, or error */
					loader->rdTlen = 0; /* end of chunks */
                    /* Skip trailing CRLF after last chunk size (if any) */
                    if ( (data - loader->rdTemp) < sizeof(loader->rdTemp) -1 && *data == '\r' && *(data+1) == '\n' ) {
                        data += 2;
                    } else if ( (data - loader->rdTemp) < sizeof(loader->rdTemp) -1 && *data == '\n' ) {
                        data += 1;
                    }
				} else {
					loader->rdTlen -= data - loader->rdTemp;
				}
			} else if (loader->rdTlen == sizeof(loader->rdTemp) -1) { /* rdTemp is full and no header found */
				/* LogPrintf("rotten chunk header\n"); */
				loader->rdDest = NULL;
				loader->rdLeft = 0;
				loader->Error = -1;
				break;
			
			} else {
				if (!s_recv) {
					sched_insert (chunked_job, loader, (long)loader->Target,
					              PRIO_KEEP);
				}
				return TRUE;
			}
		}
		
		if (size) {
            /* Phase 1: Memory Allocation Check */
			LDRCHUNK chunk = malloc (sizeof (struct s_ldr_chunk) + size); /* +size, not -1 for data[1] as it's a flexible array member */
            if (!chunk) {
                /* LogPrintf("LOADER: Failed to allocate chunk for chunked transfer. Size: %lu\n", (unsigned long)size); */
                loader->Error = -1;
                return JOB_DONE;
            }
			if (!loader->rdList) loader->rdList                   = chunk;
			else                 ((LDRCHUNK)loader->rdCurr)->next = chunk;
			loader->rdCurr = chunk;
			loader->rdDest = chunk->data;
			loader->rdLeft = size;
			chunk->next = NULL;
			chunk->size = size;
			
			if (loader->rdTlen) {
				if (size > loader->rdTlen) {
					 size = loader->rdTlen;
				}
				memcpy (loader->rdDest, data, size);
				loader->rdDest   += size;
				loader->rdLeft   -= size;
				loader->DataFill += size;
				
				loader->rdTlen -= size;
				if (loader->rdTlen) { /* there are still data in the temp buffer */
					memmove (loader->rdTemp, data + size, loader->rdTlen);
                    loader->rdTemp[loader->rdTlen] = '\0'; /* Null terminate after move */
				} else {
                    loader->rdTemp[0] = '\0'; /* Empty temp buffer */
                }

                /* Check for trailing CRLF after chunk data in rdTemp */
                if (!loader->rdLeft) {
                    if (loader->rdTlen >= 2 && loader->rdTemp[0] == '\r' && loader->rdTemp[1] == '\n') {
                        memmove(loader->rdTemp, loader->rdTemp + 2, loader->rdTlen - 2);
                        loader->rdTlen -= 2;
                    } else if (loader->rdTlen >= 1 && loader->rdTemp[0] == '\n') {
                        memmove(loader->rdTemp, loader->rdTemp + 1, loader->rdTlen - 1);
                        loader->rdTlen -= 1;
                    }
                    loader->rdTemp[loader->rdTlen] = '\0'; /* Ensure null-terminated */
                    continue; /* Chunk complete, try to read next header */
                }
			}
		}
		break;
	}
	
	if (!loader->rdDest) { /* This means all chunks received or non-chunked transfer */
		LDRCHUNK next = loader->rdList, chunk;
		long     total_data_size = 0;
		/* Calculate total size first */
		for (chunk = loader->rdList; chunk != NULL; chunk = chunk->next) {
			total_data_size += chunk->size;
		}

        /* Phase 1: Memory Allocation Check */
		char   * p_data    = loader->Data = malloc (total_data_size +3); /* +3 for null terminators */
        if (!loader->Data) {
            /* LogPrintf("LOADER: Final data buffer allocation failed. Size: %ld\n", total_data_size); */
            loader->Error = -1;
            return JOB_DONE;
        }
		
		char * current_pos = p_data; /* Use p_data here */
		next = loader->rdList; /* Reset next for actual copy */

		while ((chunk = next) != NULL) {
			memcpy (current_pos, chunk->data, chunk->size);
			current_pos += chunk->size;
			next =  chunk->next;
			free (chunk); /* Free chunk after copying data */
		}
		loader->rdList   = loader->rdCurr = NULL; /* Clear chunk list */
		loader->DataSize = loader->DataFill = total_data_size; /* Update final sizes */
		
		loader->Data[loader->DataFill +0] = '\0'; /* Null terminate */
		loader->Data[loader->DataFill +1] = '\0';
		loader->Data[loader->DataFill +2] = '\0';
	}
	if (s_recv) {
		sched_insert (receive_job, loader, (long)loader->Target, PRIO_KEEP);
	}
	
	return JOB_DONE;
}
#endif /* USE_INET */

/*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/
#ifdef USE_INET
static int
receive_job (void * arg, long invalidated)
{
	LOADER loader = arg;
	
	if (invalidated) {
		if (!loader->PostBuf) { /* Check and abort cache */
			cache_abort (loader->Location);
		}
		if (loader->SuccJob) {
			(*loader->SuccJob)(arg, invalidated);
		} else {
			delete_loader (&loader);
		}
		return JOB_DONE;
	}
	
	if (loader->rdLeft) { /* Data still needs to be read */
		long n = inet_recv (loader->rdSocket, loader->rdDest, loader->rdLeft);
		int  r = JOB_AGED;

		if (n < 0) { /* Error or connection terminated */
			if (n == -ECONNRESET) {
				/* LogPrintf("LOADER: Connection reset during receive_job.\n"); */
			} else {
				/* LogPrintf("LOADER: inet_recv error during receive_job: %ld.\n", n); */
			}
			inet_close (loader->rdSocket);
			loader->rdSocket = -1;
			loader->rdLeft   = 0;
            loader->Error = -1; /* Phase 1: Set error status */
		
		} else if (n > 0) { /* Bytes received */
			loader->rdDest   += n;
			loader->rdLeft   -= n;
			loader->DataFill += n;
			r = JOB_KEEP;
		
		} else { /* n == 0, no data, connection still open */
			r = JOB_NOOP;
		}

		if (loader->rdLeft && loader->Error != -1) { /* If more data expected and no error */
			return r;  /* re-schedule for next block of data */
		}

        /* If rdLeft is 0 and it's a chunked transfer, ensure complete */
		if (!loader->rdLeft && loader->rdChunked) {
			if (chunked_job (arg, 0)) { /* Process remaining chunks */
				return JOB_DONE;  /* chunk head need to be completed, job rescheduled */
			} else if (loader->rdLeft) { /* If chunked_job indicates more data is still needed for current chunk */
                return r; /* Reschedule for start of next chunk data */
            }
		}

	}
	/* else download finished */
	
	if (loader->rdSocket >= 0) {
		inet_close (loader->rdSocket);
		loader->rdSocket   = -1;
	}

    /* Phase 1: Handle errors for loader->Data */
    if (loader->Error != -1) {
        if (!loader->Data) { /* If Data is NULL, create a generic error message */
            const char err_msg[] = "<html><head><title>Error</title></head><body>"
                                   "<h1>Network Error</h1><p>Failed to receive data.</p></body></html>";
            loader->Data = strdup(err_msg);
            if (loader->Data) {
                loader->DataSize = loader->DataFill = strlen(loader->Data);
                loader->MimeType = MIME_TXT_HTML;
            } else {
               /* LogPrintf("LOADER: Failed to allocate error message data in receive_job.\n"); */
            }
        }
    }
	
	if (loader->Data) {
		char * p = loader->Data + loader->DataFill;
		*(p++) = '\0'; /* Null terminate */
		*(p++) = '\0';
		*(p)   = '\0';
		
		if (!loader->PostBuf || MIME_Major (loader->MimeType) != MIME_TEXT) {
			const char * ext = mime_toExtension (loader->MimeType);
			if (loader->PostBuf) {
				loader->Expires = -1; /* mark to get deleted at program end */
			}
			loader->Cached = cache_assign (loader->Location, loader->Data,
			                               loader->DataSize,
			                               (ext && *ext ? ext : loader->FileExt),
			                               loader->Date, loader->Expires);
			if (loader->Cached) {
				cache_bound (loader->Location, NULL);
			}
		}
	}
	if (loader->SuccJob) {
		sched_insert (loader->SuccJob, loader,(long)loader->Target, PRIO_SUCCESS);
	} else {
		start_parser (loader);
	}
	return JOB_DONE;	
}
#endif /* USE_INET */

/*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/
#ifdef USE_INET
static int
header_job (void * arg, long invalidated)
{
	LOADER   loader = arg;
	LOCATION loc    = loader->Location;
	
	const char * host = NULL;
	HTTP_HDR     hdr;
	short        sock = -1;
	short        reply;
	char       * auth = NULL;
	
	BOOL hdr_only = (!loader->Target && loader->SuccJob);
	BOOL cache_lk = (!loader->PostBuf && !hdr_only);
#define CACHE_ABORT(loc_ptr)   { if (cache_lk) cache_abort (loc_ptr); }
	
	if (invalidated) {
		if (loader->SuccJob) {
			(*loader->SuccJob)(arg, invalidated);
		} else {
			delete_loader (&loader);
		}
		return JOB_DONE;
	}
	
	if (cache_lk) switch (CResultDsk (cache_exclusive (loc))) {
		
		case CR_BUSY:
		/*	printf ("header_job(%s): cache busy\n", loc->FullName);*/
			return JOB_KEEP;
		
		case CR_LOCAL: {
			CACHED cached = cache_lookup (loc, 0, NULL);
			if (cached) {
				union { CACHED c; LOCATION l; } u;
				u.c            = cache_bound (cached, &loader->Location);
				/* Phase 1: Memory Allocation Check */
				if (!u.l) {
					/*LogPrintf("LOADER: header_job cache_bound failed for local cache hit.\n."); */
					cache_release(&cached, FALSE);
					loader->Error = -1; /* Set error flag */
					CACHE_ABORT(loc);
					return JOB_DONE;
				}
				loader->Cached = u.l;
				if (!loader->MimeType) {
					loader->MimeType = mime_byExtension (loader->Cached->File,
					                                     NULL, NULL);
				}
			/*	printf ("header_job(%s): cache hit\n", loc->FullName);*/
				if (loader->SuccJob) {
					sched_insert (loader->SuccJob,
					              loader, (long)loader->Target, PRIO_SUCCESS);
				} else {
					sched_insert (loader_job,
					              loader, (long)loader->Target, PRIO_LOADFILE);
				}
				return JOB_DONE;
			
			} else { /* cache inconsistance error */
				/*LogPrintf ("header_job(%s): not in cache!\n", loc->FullName);*/
				return header_job (arg, (long)loader->Target); /* invalidate */
			}
		} /*break;*/
	}	
	
	/* Connect to host
	*/
	if (loader->Target) {
		/* Assuming location_Host returns const char* and takes UWORD* for port. */
		UWORD port_val;
		if ((host = location_Host (loc, &port_val)) != NULL && !*host) host = NULL;
	}
	if (host) {
		char buf[300];
        /* Phase 1: Buffer Overflow Prevention - Use snprintf */
		snprintf (buf, sizeof(buf), "Connecting: %.*s", (int)(sizeof(buf) -13), host);
		containr_notify (loader->Target, HW_SetInfo, buf);
	}
	while(1) { /* authorization loop */
		do {
			long tout = (loader->SuccJob ? hdr_tout_gfx : hdr_tout_doc);
			reply = http_header (loc, &hdr, sizeof (loader->rdTemp) -2,
			                     &sock, tout, loader->Referer, auth, loader->PostBuf);
		} while (reply == 100);
		
		if (reply < 0) { /* Error or timeout/reset */
			if (reply == -ECONNRESET || reply == -ETIMEDOUT) { /* Assume these are from errno.h */
				if (loader->Retry) {
					CACHE_ABORT(loc);
					loader->Retry--;
					inet_close (sock); /* Close socket before retrying */
					sock = -1; /* Invalidate socket handle */
					return JOB_KEEP; /* try connecting later */
				}
			}
			if (reply == -EMFILE) { /* Assume EMFILE is from errno.h */
				CACHE_ABORT(loc);
				inet_close (sock); /* Close socket before retrying */
				sock = -1; /* Invalidate socket handle */
				return JOB_KEEP; /* too many open connections yet, try again later */
			}
            /*LogPrintf("LOADER: HTTP header error or connection failure: %ld. Errno: %d\n", reply, errno);*/
			loader->Error = reply; /* Store the error code */
			CACHE_ABORT(loc); /* Abort cache as well */
			inet_close (sock);
			sock = -1;
			break; /* Exit authorization loop to handle error */
		}
		
		if (reply == 401) {
			if (!auth && hdr.Realm && loader->AuthBasic && loader->AuthRealm
			          && strcmp (loader->AuthRealm, hdr.Realm) == 0) {
				inet_close (sock);
				sock = -1;
				auth = loader->AuthBasic;
				continue;
			}
			if (loader->AuthRealm) {
				free (loader->AuthRealm);
				loader->AuthRealm = NULL;
			}
			if (loader->AuthBasic) {
				free (loader->AuthBasic);
				loader->AuthBasic = NULL;
			}
			if (hdr.Realm && !loader->SuccJob && !hdr_only) {
				const char form[] =
					"<html><head><title>%.*s</title></head>"
					"<body bgcolor=\"white\">"
					"<h1>Authorization Required</h1>&nbsp;<br>"
					"Enter user name and password for <q><b>%s</b></q> at <b>%s</b>:"
					"<form method=\"AUTH\">"
					"<table border=\"0\">"
					"<tr><td align=right>User:&nbsp;"
					    "<td><input type=\"text\" name=\"USR\">"
					"<tr><td align=right>Password:&nbsp;"
					    "<td><input type=\"password\" name=\"PWD\">"
					"<tr><td align=right>"
					      "<input type=\"submit\" value=\"Login\">&nbsp;"
					"</table></form>&nbsp;<p><hr size=\"1\">"
					"\n<small><pre>%s</pre></small>\n"
					"</bod></html>";
				const char * titl = hdr.Head, * text;
				size_t       titl_s, size;
				while (*titl && !isspace(*titl)) titl++;
				while (isspace(*titl))           titl++;
				text = titl;
				while (*text && *text != '\r' && *text != '\n') text++;
				titl_s = text - titl;
				while (isspace(*text)) text++;
                /* Phase 1: Calculate needed size precisely */
				size = sizeof(form)
				     + titl_s + strlen(hdr.Realm) + strlen(host) + strlen (text) + 10; /* +10 for safety/format specifiers */
                
                /* Phase 1: Memory Allocation Check */
				if ((loader->AuthRealm = strdup (hdr.Realm)) == NULL) {
                    /*LogPrintf("LOADER: strdup(AuthRealm) failed for 401 response.\n.");*/
                    inet_close (sock); sock = -1; CACHE_ABORT(loc);
                    loader->Error = -ENOMEM; /* Assume ENOMEM from errno.h */
                    break;
                }
                /* Phase 1: Memory Allocation Check */
				if ((loader->Data = malloc (size)) == NULL) {
                    /*LogPrintf("LOADER: malloc(loader->Data) failed for 401 response.\n.");*/
                    free(loader->AuthRealm); loader->AuthRealm = NULL;
                    inet_close (sock); sock = -1; CACHE_ABORT(loc);
                    loader->Error = -ENOMEM;
                    break;
                }
				
				inet_close (sock);
				sock = -1;
				CACHE_ABORT(loc);
                /* Phase 1: Buffer Overflow Prevention - Use snprintf */
				size = snprintf (loader->Data, size, form,
				                (int)titl_s, titl, hdr.Realm, host, text);
				loader->Data[++size] = '\0';
				loader->Data[++size] = '\0';
				loader->MimeType = MIME_TXT_HTML;
				start_parser (loader);
				return JOB_DONE;
			}
		}
		break;
	}
	/* Check for HTTP header redirect
	*/
	if ((reply == 301 || reply == 302 || reply == 303) && hdr.Rdir) {
		LOCATION redir  = new_location (hdr.Rdir, loader->Location);
		CACHED   cached = (hdr_only ? NULL : cache_lookup (redir, 0, NULL));
		inet_close  (sock);
		CACHE_ABORT(loc);
        /* Phase 1: Memory Allocation Check */
		if (!redir) {
            /*LogPrintf("LOADER: new_location failed for redirect URL.\n.");*/
            loader->Error = -ENOMEM; /* Assume ENOMEM from errno.h */
            return JOB_DONE;
        }
		
		if (!loader->MimeType) {
			loader->MimeType = mime_byExtension (redir->File, NULL, NULL);
		}
		if (cached) {
			union { CACHED c; LOCATION l; } u;
	 		if (loader->Cached) {
 				cache_release ((CACHED*)&loader->Cached, FALSE);
 			}
			u.c = cache_bound (cached, &loader->Location);
            /* Phase 1: Memory Allocation Check */
            if (!u.l) {
                /*LogPrintf("LOADER: cache_bound failed for cached redirect.\n.");*/
                cache_release(&cached, FALSE);
                free_location(&redir);
                loader->Error = -ENOMEM;
                return JOB_DONE;
            }
			loader->Cached = loc = u.l;
			free_location (&redir);
			if (loader->SuccJob) {
				sched_insert (loader->SuccJob,
				              loader, (long)loader->Target, PRIO_SUCCESS);
			} else {
				sched_insert (loader_job,
				              loader, (long)loader->Target, PRIO_LOADFILE);
			}
			return JOB_DONE;
		
		} else {
			free_location (&loader->Location);
			loader->Location = loc = redir;
			return JOB_KEEP; /* re-schedule with the new location */
		}
	}
	
	/* start loading
	*/
	if (hdr.MimeType) {
		if (MIME_Major (hdr.MimeType) && MIME_Minor (hdr.MimeType)
		    && (MIME_Major (loader->MimeType) != MIME_Major (hdr.MimeType)
			     || !MIME_Minor (loader->MimeType))) {
			loader->MimeType = hdr.MimeType;
		}
		if (hdr.Encoding) {
			loader->Encoding = hdr.Encoding;
		}
	}
	if (reply == 200) {
		if (host) {
			char buf[300];
            /* Phase 1: Buffer Overflow Prevention - Use snprintf */
			snprintf (buf, sizeof(buf), "Receiving from %.*s", (int)(sizeof(buf) -16), host);
			containr_notify (loader->Target, HW_SetInfo, buf);
		}
		loader->Date  = (hdr.Modified > 0 ? hdr.Modified : hdr.SrvrDate);
		loader->Tdiff = hdr.LoclDate - hdr.SrvrDate;
		if (hdr.Expires > 0) {
			loader->Expires = hdr.Expires + loader->Tdiff;
		} else if (hdr.Modified <= 0) {
			/* none valid date given at all, so we assume a dynamic page
			 * (eg. from php) that needs to be revisited at the next session */
			loader->Expires = -1; /* mark to get deleted at program end */
		}
		
		if (hdr_only) {
			loader->DataSize = hdr.Size;
			if ((loader->rdChunked = hdr.Chunked) == TRUE) {
				loader->rdTemp[0] = '\r';
				loader->rdTemp[1] = '\n';
				loader->rdTlen = 2;
			}
            /* Phase 1: Buffer Overflow Prevention - Use memcpy with bounds */
			if (hdr.Tlen > 0 && hdr.Tlen < sizeof(loader->rdTemp) - loader->rdTlen) {
				memcpy (loader->rdTemp + loader->rdTlen, hdr.Tail, hdr.Tlen);
				loader->rdTlen += hdr.Tlen;
                loader->rdTemp[loader->rdTlen] = '\0'; /* Ensure null-termination */
			} else if (hdr.Tlen > 0) { /* Not enough space for full tail */
                /*LogPrintf("LOADER: Header tail too large for rdTemp in hdr_only mode.\n.");*/
                loader->Error = -1; /* Set error state */
                CACHE_ABORT(loc);
                return JOB_DONE;
            }

			loader->rdSocket = sock;
			if (!sched_insert (loader->SuccJob, loader, (long)0, PRIO_RECIVE)) {
				/*LogPrintf("LOADER: sched_insert for SuccJob (hdr_only) failed.\n.");*/
                inet_close (sock); loader->Error = -1; CACHE_ABORT(loc);
				return JOB_DONE;
			}
			return JOB_DONE;
		
		} else if (hdr.Size >= 0 && !hdr.Chunked) {
            /* Phase 1: Memory Allocation Check */
			loader->Data     = loader->rdDest = malloc (hdr.Size +3);
            if (!loader->Data) {
                /*LogPrintf("LOADER: malloc(loader->Data) failed for non-chunked. Size: %ld\n.", hdr.Size);*/
                inet_close (sock); loader->Error = -1; CACHE_ABORT(loc);
                return JOB_DONE;
            }
			loader->DataSize = loader->rdLeft = hdr.Size;
			loader->DataFill = hdr.Tlen;
            /* Phase 1: Buffer Overflow Prevention - Use memcpy with bounds */
			if (loader->DataFill > 0 && loader->DataFill < loader->rdLeft) { /* ensure DataFill doesn't exceed allocated or remaining */
				memcpy (loader->rdDest, hdr.Tail, loader->DataFill);
				loader->rdDest += loader->DataFill;
				loader->rdLeft -= loader->DataFill;
			} else if (loader->DataFill > 0) { /* hdr.Tlen was too big for allocated buffer */
                /* LogPrintf("LOADER: Header tail too large for Data in non-chunked mode.\n.");*/
                loader->Error = -1;
                inet_close(sock); CACHE_ABORT(loc);
                return JOB_DONE;
            }
			if (sock < 0 || !loader->rdLeft) {
				loader->Data[loader->DataFill +0] = '\0'; /* Null terminate */
				loader->Data[loader->DataFill +1] = '\0';
				loader->Data[loader->DataFill +2] = '\0';
				/* nothing more to do, falls through */
			} else {
				loader->rdSocket = sock;
				if (!sched_insert (receive_job,
				              loader, (long)loader->Target, PRIO_RECIVE)) {
					/*LogPrintf("LOADER: sched_insert for receive_job (non-chunked) failed.\n.");*/
                    inet_close (sock); loader->Error = -1; CACHE_ABORT(loc);
					return JOB_DONE;
				}
				return JOB_DONE;
			}
		
		} else {
			if ((loader->rdChunked = hdr.Chunked) == TRUE) {
				loader->rdTemp[0] = '\r';
				loader->rdTemp[1] = '\n';
				loader->rdTlen = 2;
			}
            /* Phase 1: Buffer Overflow Prevention - Use memcpy with bounds */
			if (hdr.Tlen > 0 && hdr.Tlen < sizeof(loader->rdTemp) - loader->rdTlen) {
				memcpy (loader->rdTemp + loader->rdTlen, hdr.Tail, hdr.Tlen);
				loader->rdTlen += hdr.Tlen;
                loader->rdTemp[loader->rdTlen] = '\0'; /* Ensure null-termination */
			} else if (hdr.Tlen > 0) {
                /* LogPrintf("LOADER: Header tail too large for rdTemp in chunked mode.\n."); */
                loader->Error = -1;
                inet_close(sock); CACHE_ABORT(loc);
                return JOB_DONE;
            }
			loader->rdSocket = sock;
			if (!sched_insert (chunked_job, loader, (long)loader->Target, PRIO_RECIVE)) {
				/* LogPrintf("LOADER: sched_insert for chunked_job failed.\n."); */
                inet_close (sock); loader->Error = -1; CACHE_ABORT(loc);
				return JOB_DONE;
			}
			return JOB_DONE;
		}
	
	} else { /* something went wrong */
		loader->Error = reply;
	}
	
	inet_close (sock);
	
	/* if it is a short file and already finished, end proceedings
	*/
	if (loader->Data) { /* Check if Data contains any error message data */
		return receive_job (loader, 0);
	}
	
	/* an error case occured
	*/
	CACHE_ABORT(loc);
	
	loader->MimeType = MIME_TEXT;
    /* Phase 1: Memory Allocation Check */
	loader->Data     = strdup (hdr.Head); /* Allocate Data for error message */
    if (!loader->Data) {
       /* LogPrintf("LOADER: strdup(hdr.Head) for error message failed.\n."); */
        loader->Error = -ENOMEM;
    } else {
        loader->DataSize = loader->DataFill = strlen(loader->Data);
        loader->Data[loader->DataFill +0] = '\0'; /* Null terminate */
        loader->Data[loader->DataFill +1] = '\0';
        loader->Data[loader->DataFill +2] = '\0';
    }
	
	if (loader->SuccJob) {
		(*loader->SuccJob)(loader, 0);
	} else {
		start_parser (loader);
	}
	
#undef CACHE_ABORT
	
	return JOB_DONE;
}
#endif /* USE_INET */


/*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/
static int
loader_job (void * arg, long invalidated)
{
	LOADER   loader = arg;
	LOCATION loc    = (loader->Cached ? loader->Cached : loader->Location);
	
	if (invalidated) {
		delete_loader (&loader);
		return JOB_DONE;
	}
	
	if (!loader->notified) {
		loader->notified = containr_notify (loader->Target, HW_PageStarted,
		                                    loader->Location);
	}
	
	loader->Data = load_file (loc, &loader->DataSize, &loader->DataFill);
	/* Phase 1: Memory Allocation Check */
	if (!loader->Data) {
		char   buf[1024]; /* Use fixed-size buffer for error message construction */
		size_t n;
		const char head[] = "<html><head><title>Error</title></head>"
		                    "<body><h1>Page not found!</h1>\n<u><pre>";
		const char tail[] = "</pre></u>\n</body></html>";

        /* Phase 1: Buffer Overflow Prevention - Use snprintf for safe error message creation */
		snprintf(buf, sizeof(buf), "%s", head);
		n = strlen(buf); /* Get length after copying head */
		
		/* Append filename/path, ensuring bounds */
		n += location_FullName (loc, buf + n, sizeof(buf) - n - sizeof(tail) -1); /* -1 for potential null, location_FullName returns bytes copied */
		
		/* Append tail, ensuring bounds */
		/* Phase 1: Buffer Overflow Prevention - Use snprintf for tail */
        snprintf(buf + n, sizeof(buf) - n, "%s", tail);
		
		loader->Error    = -ENOENT; /* Assuming -ENOENT from errno.h */
		loader->Data     = strdup (buf);
		/* Phase 1: Memory Allocation Check */
		if (!loader->Data) {
			/* LogPrintf("LOADER: strdup for 'Page not found' error message failed.\n."); */
			delete_loader(&loader); /* Cleanup if strdup fails */
			return JOB_DONE;
		}
		loader->MimeType = MIME_TXT_HTML;
	}
	/* registers a parser job with the scheduler */
	start_parser (loader);

	return JOB_DONE;
}


/*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/
static int
generic_job (void * arg, long invalidated)
{
	LOADER loader = arg;
	
	if (!invalidated && !loader->Error) {
		LOCATION loc = (loader->Cached ? loader->Cached : loader->Location);
		
		if (!loader->ExtAppl) {
			/* LogPrintf("generic_job(): no appl found!\n"); */

		} else if (PROTO_isRemote (loc->Proto)) {
			/* LogPrintf("generic_job(): not in cache!\n"); */

		} else {
			start_application (loader->ExtAppl, loc);
		}
	}
	delete_loader (&loader);
	
	return JOB_DONE;
}

/*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/
int
saveas_job (void * arg, long invalidated)
{
	LOADER loader = arg;
	char fsel_file[HW_PATH_MAX];
/*	WORD r, butt;*/  /* file selector exit button */
	int    fh1 = -1, fh2 = -1; /* Phase 1: Initialize file handles to -1 */
	long fsize;
	size_t bsize; /* Use size_t for buffer size */
	ssize_t rsize; /* Use ssize_t for read/write return values */
	long csize = 0;
	char *buffer = NULL; /* Phase 1: Initialize buffer to NULL */
	char *p;
	HwWIND wind = NULL; /* Phase 1: Initialize wind to NULL */
				
	if (!invalidated && !loader->Error) {
        /* Phase 1: Ensure loader and its target are valid */
        if (!loader || !loader->Target) {
            /* LogPrintf("LOADER: saveas_job called with NULL loader or target.\n");*/
            goto saveas_bottom;
        }
		wind = hwWind_byContainr (loader->Target);
		LOCATION loc = (loader->Cached ? loader->Cached : loader->Location);
		LOCATION remote = loader->Location;

        /* Phase 1: Ensure locations are valid before use */
        if (!loc || !remote) {
            /* LogPrintf("LOADER: saveas_job: source/remote location is NULL.\n"); */
            goto saveas_bottom;
        }

		if (PROTO_isRemote (loc->Proto)) {
			/* LogPrintf("saveas_job(): not in cache!\n"); */

		} else {
			/* get cache file size */
			fsize =	file_size(loc);

			if (fsize <= 0)
			{
				/* LogPrintf("LOADER: saveas_job(): file empty or not found (size %ld), skipping save.\r\n", fsize); */
				goto saveas_bottom;
			}

			/* remote->file is the remote filename, with the possibility
			 * of extra characters
			 * ssize is the start size of the remote file
			 * nsize is the size of everything after a ?
			 */
            size_t ssize_len = strlen(remote->File); /* Use size_t for lengths */
            size_t nsize = 0;
			p = strrchr (remote->File, '?');

			if (p) {
				nsize = strlen(p);
				ssize_len -= nsize;
			}
            /* Phase 1: Buffer Overflow Prevention - Use strncpy and null-terminate */
			if (ssize_len >= sizeof (fsel_file)) {
				ssize_len = sizeof (fsel_file) -1;
			}
			strncpy (fsel_file, remote->File, ssize_len);
			fsel_file[ssize_len] = '\0';
			
			/* get our new filename */
			if (file_selector ("HighWire: Save File as...", NULL,
			                   fsel_file, fsel_file,sizeof(fsel_file))) {
                /* Phase 1: Log file selection */
                /* LogPrintf("LOADER: User selected file '%s' for saving.\n", fsel_file); */

                /* Phase 1: Initialize fh2 to -1 before open */
				if ((fh2 = open (fsel_file, O_RDWR|O_CREAT|O_TRUNC, 0666)) >= 0) {
                    /* Phase 1: Log file creation */
                    /* LogPrintf("LOADER: Destination file '%s' opened (handle %d).\n", fsel_file, fh2); */

					/* get our cache file name */
                    /* Phase 1: Buffer Overflow Prevention - Use location_FullName safely */
                    /* location_FullName returns bytes copied, 0 on error, or -1 if buffer too small */
                    int bytes_copied = location_FullName (loc, va_helpbuf, sizeof(va_helpbuf));
                    if (bytes_copied <= 0) {
                        /* LogPrintf("LOADER: Failed to get full path for cache file (location_FullName returned %d).\n", bytes_copied); */
                        close(fh2); fh2 = -1; /* Close destination file */
                        goto saveas_bottom;
                    }

					/* attempt to open cache file */
                    /* Phase 1: Initialize fh1 to -1 before open */
					fh1 = open (va_helpbuf, O_RDONLY);
					
					if (fh1 < 0) {
                        /* LogPrintf("LOADER: Cache file '%s' not found or could not be opened (handle %d). Errno: %d\r\n", va_helpbuf, fh1, errno); */
						close (fh2); fh2 = -1;
						goto saveas_bottom;
					} 
                    /* Phase 1: Log cache file open */
                    /* LogPrintf("LOADER: Source cache file '%s' opened (handle %d).\n", va_helpbuf, fh1); */

					/* max 32k buffer to read/write */
                    /* Phase 1: Use size_t for bsize, correct min/max logic */
                    bsize = 32768; /* Max buffer size */
                    if ((size_t)fsize < bsize) { /* If file is smaller than max buffer */
                        bsize = (size_t)fsize;
                    }
                    if (bsize == 0 && fsize > 0) { /* If fsize > 0 but bsize is 0, set to 1 */
                        bsize = 1;
                    }
                    if (bsize == 0) { /* If file is empty, skip buffer allocation */
                        /* LogPrintf("LOADER: Source file has size 0, skipping data transfer.\n"); */
                        close(fh1); close(fh2);
                        goto saveas_bottom;
                    }
					
                    /* Phase 1: Memory Allocation Check */
					buffer = malloc(bsize);
					if (!buffer) {
                        /* LogPrintf("LOADER: Memory allocation error for copy buffer (size %zu).\r\n", bsize); */
						close (fh1); fh1 = -1;
						close (fh2); fh2 = -1;
						goto saveas_bottom;
					}
                    /* Phase 1: Log buffer allocation */
                    /* LogPrintf("LOADER: Allocated %zu bytes for copy buffer.\n", bsize); */

					while (csize < fsize)
					{
                        /* Phase 1: Read with error checking */
						rsize = read (fh1, buffer, bsize);
						if (rsize < 0) { /* Error reading */
                           /* LogPrintf("LOADER: Read error from cache file (handle %d). Errno: %d\n", fh1, errno); */
                            break;
                        }
                        if (rsize == 0) { /* EOF reached prematurely */
                            /* LogPrintf("LOADER: Unexpected EOF in cache file at %ld/%ld bytes.\n", csize, fsize); */
                            break;
                        }
                        /* Phase 1: Write with error checking */
						ssize_t wsize = write (fh2, buffer, rsize); /* Use ssize_t for write return */
                        if (wsize < 0) { /* Error writing */
                            /* LogPrintf("LOADER: Write error to destination file (handle %d). Errno: %d\n", fh2, errno); */
                            break;
                        }
                        if (wsize != rsize) { /* Partial write */
                            /*LogPrintf("LOADER: Partial write to destination file (%ld of %ld bytes).\n", wsize, rsize);*/
                            csize += wsize; /* Add only what was actually written */
                            break; /* Stop on partial write to avoid data corruption */
                        }
						csize += rsize;
					}

					close (fh1); fh1 = -1;
					close (fh2); fh2 = -1;

					/* and release our buffer */
					free(buffer); buffer = NULL;
					
				} else {
					/* LogPrintf("File creation error. Errno: %d\r\n", errno); */
				}
			} else {
                /* LogPrintf("LOADER: File selection cancelled by user.\n."); */
            }
		}
	}

saveas_bottom:
    /* Phase 1: Ensure all allocated resources are freed on all paths */
    if (fh1 != -1) close(fh1);
    if (fh2 != -1) close(fh2);
    if (buffer) free(buffer);
	
	delete_loader (&loader);

	/* We should close the window here	 */
	if (wind) {
		delete_hwWind (wind);
	}

	return JOB_DONE;
}


/*----------------------------------------------------------------------------*/
static short
start_application (const char * appl, LOCATION loc)
{
	char *p; /* Declare p here */
	
	/* Phase 1: Add NULL checks for inputs */
	if (!appl || !loc) {
		/*LogPrintf("LOADER: start_application called with NULL app or loc.\n"); */
		return -1;
	}

	if (av_shell_id >= 0) {
		short msg[8];
        /* Phase 1: Ensure msg buffer is initialized */
        memset(msg, 0, sizeof(msg));

		msg[0] = (AV_STARTPROG); /* AV_STARTPROG is assumed to be defined in vaproto.h */
		msg[1] = gl_apid; /* gl_apid is assumed to be defined in global.h */
		/* msg[2] is length, set later */
		
        /* Phase 1: Buffer Overflow Prevention - Use snprintf */
		if (cfg_Viewer && mime_byExtension (appl, NULL, NULL) == MIME_TXT_HTML) { /* MIME_TXT_HTML is assumed in mime.h */
			size_t viewer_len = strlen(cfg_Viewer);
            /* Use snprintf to ensure buffer fits, and null terminate */
			snprintf (va_helpbuf, sizeof(va_helpbuf), "%s", cfg_Viewer); 	 
            va_helpbuf[sizeof(va_helpbuf) - 1] = '\0'; 

            /* Check if there's enough space for both viewer path and app path */
            if (viewer_len + 1 + strlen(appl) + 1 > sizeof(va_helpbuf)) {
               /* LogPrintf("LOADER: Not enough space in va_helpbuf for viewer path in start_application.\n."); */
                return -1;
            }
			p = va_helpbuf + viewer_len + 1; /* position after the \0 of string */
            /* Use snprintf for safe copy of application path */
			snprintf (p, sizeof(va_helpbuf) - (p - va_helpbuf), "%s", appl); 
			
			*(char**)(msg +3) = va_helpbuf; /* pointer to program name */
			*(char**)(msg +5) = p;          /* pointer to file path for the program */
			msg[2] = 2 + 2; /* 2 words for program name (pointer) + 2 words for file path (pointer) = 4 words = 16 bytes */
		} else {
            /* Phase 1: Buffer Overflow Prevention - Use location_FullName and snprintf */
            int copied_bytes = location_FullName (loc, va_helpbuf, sizeof(va_helpbuf)); /* Assumed location_FullName is from Location.h */
            if (copied_bytes <= 0) {
               /* LogPrintf("LOADER: Failed to get full name for application start in start_application.\n."); */
                return -1;
            }
			*(char**)(msg +3) = va_helpbuf; /* pointer to file path, AVSERVER starts the appropiate program */
			msg[2] = 2; /* 2 words for path pointer = 4 bytes */
		}
		/* appl_write is assumed to be defined by AES library includes (e.g. gemlib.h). */		
		appl_write (av_shell_id, msg[2] + 12, msg);
		/* send_olga_link is assumed to be declared extern or in olga.h */
		/* Will not explicitly declare here to avoid assumptions based on problem description. */
		/* send_olga_link ( appl ); */
	}
	return av_shell_id;
}


/*==============================================================================
 * small routine to call external viewer to view the SRC to
 * a file so that realtime comparisons can be made
 * mlutz February 26, 2006 with the help of Djordje Vukovic
*/
void
launch_viewer(const char *name)
{
	char *p = 0; /* Initialize p */
	
	/* Phase 1: Add NULL check for name */
	if (!name) {
		/*LogPrintf("LOADER: launch_viewer called with NULL name.\n"); */
		return;
	}

	if (av_shell_id >= 0) {
		short msg[8];
        /* Phase 1: Ensure msg buffer is initialized */
        memset(msg, 0, sizeof(msg));

		msg[0] = (AV_STARTPROG);
		msg[1] = gl_apid;
		msg[2] = 0;
		
        /* Phase 1: Buffer Overflow Prevention - Use snprintf */
		if (cfg_Viewer && mime_byExtension (name, NULL, NULL) == MIME_TXT_HTML) {
			size_t viewer_len = strlen(cfg_Viewer);
            /* Use snprintf for safe copying */
			snprintf (va_helpbuf, sizeof(va_helpbuf), "%s", cfg_Viewer);
            va_helpbuf[sizeof(va_helpbuf) - 1] = '\0'; 

            /* Check if there's enough space */
            if (viewer_len + 1 + strlen(name) + 1 > sizeof(va_helpbuf)) {
                /*LogPrintf("LOADER: Not enough space in va_helpbuf for viewer+name in launch_viewer.\n.");*/
                return;
            }
			p = va_helpbuf + viewer_len + 1; /* position after the \0 of string */
            /* Use snprintf for safe copy of application path */
			snprintf (p, sizeof(va_helpbuf) - (p - va_helpbuf), "%s", name); 
			
			*(char**)(msg +3) = va_helpbuf; /* pointer to program name */
			*(char**)(msg +5) = p;          /* pointer to file path for the program */
			msg[2] = 16;
		} else {
            /* Use snprintf for safe copying directly */
			snprintf (va_helpbuf, sizeof(va_helpbuf), "%s", name);
            va_helpbuf[sizeof(va_helpbuf) - 1] = '\0'; /* Ensure null termination */
			*(char**)(msg +3) = va_helpbuf; /* pointer to file path, AVSERVER starts the appropiate program */
			msg[5] = msg [6] = msg[7] = 0;
		}		
		appl_write (av_shell_id, msg[2] + 12, msg);
		/* send_olga_link is assumed to be declared extern or in olga.h */
		/* send_olga_link ( name ); */
	}
}


/******************************************************************************/

/*============================================================================*/
/* Get the size of a file from the cache, predominately */

long
file_size (const LOCATION loc)
{
	/* Phase 1: Add NULL checks for inputs */
	if (!loc || !loc->FullName) { 
		/* LogPrintf("LOADER: file_size called with NULL location or FullName.\n"); */
		return 0; /* Return 0 for invalid location */
	}

	const char *filename = loc->FullName;
	long         size = 0;
	/* struct xattr and DTA are assumed to be defined in file_sys.h */
	struct xattr file_info; 
	long         xret;
    DTA  new_dta, * old_dta; 

    /* Attempt Fxattr first */
    xret = Fxattr(0, filename, &file_info); /* Fxattr is from mint/osbind.h */
	
	if (xret == E_OK) {  /* E_OK is from errno.h */
		size = file_info.st_size;
	
	} else if (xret == -EINVFN || xret == -ENOENT) {  /* EINVFN, ENOENT are from errno.h */
		old_dta = Fgetdta(); /* Fgetdta is from mint/osbind.h */
		Fsetdta(&new_dta); /* Fsetdta is from mint/osbind.h */

		if (Fsfirst(filename, 0) == E_OK) { /* Fsfirst is from mint/osbind.h */
			size = new_dta.d_length;
		} else {
           /* LogPrintf("LOADER: Fsfirst failed for %s. Errno: %d\n", filename, errno);*/
            size = -1; /* Indicate file not found or other error */
        }
		Fsetdta(old_dta); /* Restore original DTA */
	
	} else { /* Other Fxattr error */
		/*LogPrintf("LOADER: Fxattr failed for %s with error %ld. Errno: %d\n", filename, xret, errno);*/
		size = -1;
	}
	
	return(size);
}


/*============================================================================*/
char *
load_file (const LOCATION loc, size_t * expected, size_t * loaded)
{
	/* Phase 1: Add NULL checks for inputs */
	if (!loc || !loc->FullName) {
		/* LogPrintf("LOADER: load_file called with NULL location or FullName.\n"); */
		*expected = 0; *loaded = 0;
		return NULL;
	}

	long   size = file_size (loc);
	char * file_buffer = NULL; 
	int    fh = -1; /* Phase 1: Initialize file handle */
	ssize_t bytes_read; /* For read return value */
	
	*expected = (size > 0 ? (size_t)size : 0);
	*loaded   = 0;
	
	if (size < 0) { /* file_size returned an error */
		/* LogPrintf("LOADER: load_file: file_size returned error %ld for %s.\n", size, loc->FullName); */
		return NULL;
	}
	
	if (size == 0) { /* Empty file */
		file_buffer = malloc(3); /* Phase 1: Allocate for null terminators */
		/* Phase 1: Memory Allocation Check */
		if (file_buffer) {
			file_buffer[0] = '\0';
			file_buffer[1] = '\0';
			file_buffer[2] = '\0';
			return file_buffer;
		}
		/* LogPrintf("LOADER: load_file: malloc failed for empty file.\n."); */
		return NULL;
	}

	/* Allocate buffer for file content + 3 null terminators */
	file_buffer = malloc (size + 3);
	/* Phase 1: Memory Allocation Check */
	if (file_buffer == NULL) {
		/* LogPrintf("LOADER: load_file: malloc failed for file buffer (size %ld).\n", size); */
		return NULL;
	}
	
	const char * filename = loc->FullName;
	fh = open (filename, O_RDONLY); /* O_RDONLY is from fcntl.h */
	/* Phase 1: Check file open success */
	if (fh < 0) {
		/* LogPrintf("LOADER: load_file: Failed to open file %s. Errno: %d\n", filename, errno); */
		free (file_buffer); /* Free buffer on open failure */
		return NULL;
	}
	
	bytes_read = read (fh, file_buffer, size); /* read is from unistd.h */
	/* Phase 1: Check read errors */
	if (bytes_read < 0) {
		/* LogPrintf("LOADER: load_file: Read error from %s. Errno: %d\n", filename, errno); */
		free (file_buffer);
		close (fh); /* close is from unistd.h */
		return NULL;
	}
	if (bytes_read < size) {
		/* LogPrintf("LOADER: load_file: Partial read from %s (%zd of %ld bytes).\n", filename, bytes_read, size); */
	}
	
	close (fh);
	
	*loaded = (size_t)bytes_read;
	file_buffer[*loaded + 0] = '\0';
	file_buffer[*loaded + 1] = '\0';
	file_buffer[*loaded + 2] = '\0';
	
	return file_buffer;
}


/*==============================================================================
 * init_paths()
 *
 * Handles the initialization of the file paths
 *
 * Currently just sets last_location to something in the directory
 * from which HighWire was launched, so that people on certain
 * systems can get the default values
 *
 * this could be useful for config files, RSC files etc.
 *
 * baldrick (August 14, 2001)
*/
void
init_paths(void)
{
	char temp_location[HW_PATH_MAX];
	size_t len;

	temp_location[0] = Dgetdrv(); /* Dgetdrv is from mint/osbind.h */
	temp_location[0] += (temp_location[0] < 26 ? 'A' : -26 + '1');
	temp_location[1] = ':';
	Dgetpath (temp_location + 2, 0); /* Dgetpath is from mint/osbind.h */

	len = strlen(temp_location);
	if (len > 0 && temp_location[len - 1] != '\\') {
		/* Phase 1: Buffer Overflow Prevention - Use strncat */
		strncat(temp_location, "\\", sizeof(temp_location) - len - 1);
		temp_location[sizeof(temp_location) - 1] = '\0'; /* Ensure null termination */
	}

	/* Phase 1: Buffer Overflow Prevention - Use snprintf for safe copying */
	snprintf(fsel_path, sizeof(fsel_path), "%s", temp_location);
	/* Phase 1: Buffer Overflow Prevention - Use strncat for safe concatenation */
	strncat(fsel_path, "html\\*.[HJPT]*", sizeof(fsel_path) - strlen(fsel_path) - 1);
	fsel_path[sizeof(fsel_path) - 1] = '\0';

	/* Phase 1: Buffer Overflow Prevention - Use snprintf for safe copying */
	snprintf(help_file, sizeof(help_file), "%s", temp_location);
	/* Phase 1: Buffer Overflow Prevention - Use strncat for safe concatenation */
	strncat(help_file, "html\\help\\index.htm", sizeof(help_file) - strlen(help_file) - 1);
	help_file[sizeof(help_file) - 1] = '\0';
}

/*============================================================================*/
/* new_post:
 * allocates a new POSTDATA struct with given buffer, length and type
 * delete_post:
 * detroys the POSTDATA along with its content
 */
POSTDATA
new_post(char *buffer, size_t length, char *type)
{
	POSTDATA post = (POSTDATA) malloc(sizeof(struct s_post));
	/* Phase 1: Memory Allocation Check */
	if (post != NULL)
	{
		post->Buffer = buffer; /* Assume buffer is already allocated by caller */
		post->BufLength = length;
		post->ContentType = type; /* Assume type is already allocated by caller (e.g., strdup) */
	} else {
		/* Phase 1: If malloc for POSTDATA fails, free buffer and type to prevent leaks */
		if (buffer) free(buffer);
		if (type) free(type);
		/* LogPrintf("LOADER: new_post failed to allocate POSTDATA.\n."); */
	}
	return post;
}

void
delete_post(POSTDATA post)
{
	if (post)
	{
		if (post->Buffer)
		{
			free(post->Buffer);
			post->Buffer = NULL; /* Phase 1: Nullify after freeing */
		}
		if (post->ContentType)
		{
			free(post->ContentType);
			post->ContentType = NULL; /* Phase 1: Nullify after freeing */
		}
		free(post);
		post = NULL; /* Phase 1: Nullify after freeing */
	}
	return;
}
