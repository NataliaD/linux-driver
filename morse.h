/***************************************************
 * File: morse.c                                   *
 * Ein Treibermodul, mit dessen Hilfe man          *
 * ASCII in morsecode umwandeln kann und umgekehrt *
 * Authors: Natalia Duske, Melanie Remmels         *
 * *************************************************/

#define MORSE_MAJOR 0 /* sorgt fuer die Zufaellige Zuweisung einer Majornummer durch den Kernel*/
#define MORSE_MINOR 0 /* Uebersetzung Morse -> ASCII */
#define ESROM_MINOR 1 /* Uebersetzung ASCII -> Morse */


#ifndef init_MUTEX
#define init_MUTEX(mutex) sema_init(mutex, 1)
#endif	/* init_MUTEX */

/*
 * Macros to help debugging
 */

#undef PDEBUG             /* undef it, just in case */
#ifdef SCULL_DEBUG
#  ifdef __KERNEL__
     /* This one if debugging is on, and kernel space */
#    define PDEBUG(fmt, args...) printk( KERN_DEBUG "scull: " fmt, ## args)
#  else
     /* This one for user space */
#    define PDEBUG(fmt, args...) fprintf(stderr, fmt, ## args)
#  endif
#else
#  define PDEBUG(fmt, args...) /* not debugging: nothing */
#endif

#undef PDEBUGG
#define PDEBUGG(fmt, args...) /* nothing: it's a placeholder */

  /* dynamic major by default */
#ifndef MORSE_NR_DEVS
#define MORSE_NR_DEVS 2    /* fuer esrom und morse */
#endif


/*
 * The pipe device is a simple circular buffer. Here its default size
 */
#ifndef MORSE_BUFFER
#define MORSE_BUFFER 20
#endif

/*
 * The different configurable parameters
 */
extern int nopen;           /* morse.c */
extern int lbuf;	    /* morse.c */


struct morse {
        wait_queue_head_t read_queue;	   /* lese queue */
        wait_queue_head_t write_queue;     /* schreibe queue */
        char *buffer;                	   /* beginn des buffer */
        char *end_buf;			   		   /* ende des buffer*/
        int buffersize;                    /* verwendet fuer die pointer arithmetic */
        char *rp;                          /* wo gelesen werden soll */
        char *wp;			   			   /* wo geschriben werden soll */
        int nreaders; 			   		   /* anzahl der geoeffneten Lesezugriffe */
		int nwriters;            	   	   /* anzahl der geoeffneten Schreibzugriffe */
		int type;                          /* Typ MORSE_MINOR oder ESROM_MINOR */
        struct semaphore sem;              /* mutual exclusion semaphore */
        struct cdev cdev;                  /* Char device structure */
};

/* Uebersetzungstabelle ASCII <-> MORSE */
#define SIZE_TABLE 37
#define SIZE_ASCII 2
#define SIZE_MORSE 6
#define SIZE_CHARS 2

/* Lookup Table */
static char table[SIZE_TABLE][SIZE_CHARS][SIZE_MORSE] = 
{ { "A" , ".-"    } 
, { "B" , "-..."  }
, { "C" , "-.-."  }
, { "D" , "-.."   }
, { "E" , "."     }
, { "F" , "..-."  }
, { "G" , "--."   }
, { "H" , "...."  }
, { "I" , ".."    }
, { "J" , ".---"  }
, { "K" , "-.-"   }
, { "L" , ".-.."  }
, { "M" , "--"    }
, { "N" , "-."    }
, { "O" , "---"   }
, { "P" , ".--."  }
, { "Q" , "--.-"  }
, { "R" , ".-."   }
, { "S" , "..."   }
, { "T" , "-"     }
, { "U" , "..-"   }
, { "V" , "...-"  }
, { "W" , ".--"   }
, { "X" , "-..-"  }
, { "Y" , "-.--"  }
, { "Z" , "--.."  }
, { "0" , "-----" }
, { "1" , ".----" }
, { "2" , "..---" }
, { "3" , "...--" }
, { "4" , "....-" }
, { "5" , "....." }
, { "6" , "-...." }
, { "7" , "--..." }
, { "8" , "---.." }
, { "9" , "----." }
, { " " , "  "    }
} ;
