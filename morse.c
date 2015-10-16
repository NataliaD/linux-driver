/***************************************************
 * File: morse.c                                   *
 * Ein Treibermodul, mit dessen Hilfe man          *
 * ASCII in morsecode umwandeln kann und umgekehrt *
 * Authors: Natalia Duske, Melanie Remmels         *
 * *************************************************/
 
#include <linux/module.h>
#include <linux/moduleparam.h>

#include <linux/kernel.h>	/* printk(), min() */
#include <linux/sched.h>
#include <linux/slab.h>		/* kmalloc() */
#include <linux/fs.h>		/* everything... */
#include <linux/proc_fs.h>
#include <linux/errno.h>	/* error codes */
#include <linux/types.h>	/* size_t */
#include <linux/fcntl.h>
#include <linux/cdev.h>
#include <asm/uaccess.h>
#include <linux/string.h>

#include "morse.h"		/* local definitions */


int major =   MORSE_MAJOR;
int morse_minor =   0;

int nopen = MORSE_NR_DEVS;	        /* number of devices */
module_param(nopen, int, S_IRUGO);

int lbuf =  MORSE_BUFFER;	        /* buffer size */
module_param(lbuf,  int, S_IRUGO);

dev_t morse_devno;			/* Our first device number */

static struct morse *morse_devices;

/* Forward declaration */
static int spacefree(struct morse *dev);
char* convertToMorse(const char* asciiStr);
char* convertToASCII(char* morseStr);
/*
 * Open
 * struct inode -> ein stuct in dem spezielle Sachen eines files gespeichert sind, wie Zugriffsrechte, Dateityp, Groesse, Autor
 * stuct file -> geoeffnete Version des struct inode 
 */

static int morse_open(struct inode *inode, struct file *filp)
{
	struct morse *dev;

	dev = container_of(inode->i_cdev, struct morse, cdev);//3)ein Pinter auf ein struct, dass in 2) enthalten ist
														//Rueckgabewert Pointer auf den Anfang des struct, dass bei 3) angegeben ist
	// Maximale Anzahl der Connection ( = readers + writers ) soll nopen sein. Hier pruefen:
	if ((dev->nreaders + dev->nwriters) >= nopen){
          up(&dev->sem);//Semaphore wird freigegeben, da im Augenblick keine weitere Connection geoeffnet werden kann
	  return -EBUSY;	  
	}
	
	filp->private_data = dev;

	if (down_interruptible(&dev->sem))//versucht den Semaphore zubekommen, falls das nicht klappt, geht er schlafen
		return -ERESTARTSYS;
	//Semaphore ist blockiert
	if (!dev->buffer) {//wenn buffer noch nicht angelegt ist
		/* allocate the buffer */
		dev->buffer = kmalloc(lbuf, GFP_KERNEL);
		if (!dev->buffer) {//Fehler beim kmalloc
			up(&dev->sem);//Semaphore wird freigegeben
			return -ENOMEM;
		}
	}
	//Variablen anpassen/setzen/initialisieren
	dev->buffersize = lbuf;
	dev->end_buf = dev->buffer + dev->buffersize;
	dev->rp = dev->buffer; /* rp wird auf den Anfang des Buffers gesetzt */
	dev->wp = dev->buffer; /* wp wird ebenfalls auf den Beginn des Buffers gesetzt */

	/* use f_mode,not  f_flags: it's cleaner (fs/open.c tells why) */
	if (filp->f_mode & FMODE_READ){//Lesezugriff auf die Datei
		dev->nreaders++;
	}
	if (filp->f_mode & FMODE_WRITE){//Schreibzugriffe 
		dev->nwriters++;
	}
	up(&dev->sem);//Semaphore wird endgueltig freigegeben

    printk(KERN_ALERT "morse: %s Device geoeffnet, %d (r) / %d (w) aktive connections. nopen=%d, lbuf=%d\n", 
	       ((dev->type == MORSE_MINOR) ? "MORSE" : "ESROM"), dev->nreaders, dev->nwriters, nopen, lbuf);

	return nonseekable_open(inode, filp);//man sagt dem Kernel, dass man llseek() nicht unterstuetzt und er sich selber drum kuemmern soll
}


/*
 * Release / Close
 * setzt die Werte, der Schreib/Lesezugriffe zurueck 
 * buffer freigeben wenn niemand mehr zugreift
 */
static int morse_release(struct inode *inode, struct file *filp)
{
	struct morse *dev = filp->private_data;


	down(&dev->sem);//Semaphore blockieren
	if (filp->f_mode & FMODE_READ){
		dev->nreaders--;
	}
	if (filp->f_mode & FMODE_WRITE){
		dev->nwriters--;
	}
	if (dev->nreaders + dev->nwriters == 0) {
		kfree(dev->buffer);
		dev->buffer = NULL; /* the other fields are not checked on open */
	}
	up(&dev->sem);//Semaphore freigeben

	printk(KERN_ALERT "morse: %s Device geschlossen. %d (r) / %d (w) Connections uebrig.\n", 
	       ((dev->type == MORSE_MINOR) ? "MORSE" : "ESROM"), dev->nreaders, dev->nwriters);

	return 0;
}


/*
 * Data management: read -> das Anwenderprogramm liest die Daten aus dem Buffer ein
 * konvertierung der ASCII- bzw. Morse-Zeichen
 */

static ssize_t morse_read (struct file *filp, char __user *buf, size_t count,
                loff_t *f_pos)
{
	struct morse *dev = filp->private_data;
 	char *convert=NULL;
	printk(KERN_ALERT "READ: Ich lese\n");
	if (down_interruptible(&dev->sem)){//semaphore versuchen zu blockieren
		 printk(KERN_ALERT "READ: Ich bin blockiert\n");
		return -ERESTARTSYS;
	}
	//Semapohre blockiert
	while (dev->rp == dev->wp) { /* nothing to read */
		printk(KERN_ALERT "READ: Ich bin im Semaphore");
		up(&dev->sem); /* release the lock-> Semaphore wieder freigeben*/
		if (filp->f_flags & O_NONBLOCK){//0_NONBLOCK steht dafuer, dass diese Datei im Augenblick nicht vom Kernel bearbeitet werden kann 
			return -EAGAIN;
		}
		PDEBUG("\"%s\" reading: going to sleep\n", current->comm);
		if (wait_event_interruptible(dev->read_queue, (dev->rp != dev->wp)))
			return -ERESTARTSYS; /* signal: tell the fs layer to handle it */
		/* otherwise loop, but first reacquire the lock */
		if (down_interruptible(&dev->sem))
			return -ERESTARTSYS;
	}
	printk(KERN_ALERT "READ: Semaphore freigegeben, rp ist jetzt [%s](%p), wp ist [%s](%p), buf ist [%s].\n",
	       dev->rp, dev->rp, dev->wp, dev->wp, buf);

	/* ok, data is there, return something */
	if (dev->wp > dev->rp)
		count = min(count, (size_t)(dev->wp - dev->rp));
	else /* the write pointer has wrapped, return data up to dev->end_buf */
		count = min(count, (size_t)(dev->end_buf - dev->rp));
	if (copy_to_user(buf, dev->rp, count)) {
		up (&dev->sem);
		return -EFAULT;
	}
	dev->rp += count;

	if (dev->rp == dev->end_buf)
		dev->rp = dev->buffer; /* wrapped */
	//up (&dev->sem);
	
	// Endsymbol anhängen, damit strlen korrekt arbeitet
	buf[count] = '\0';
	
	/* konvertierung */
	if( MORSE_MINOR == dev->type){
          
	  printk(KERN_ALERT "READ: Konvertiere ASCII in MORSE\n");
	  convert=convertToMorse( buf );
	  
	}else if( ESROM_MINOR == dev->type){

	  printk(KERN_ALERT "READ: Konvertiere MORSE in ASCII\n");
	  convert=convertToASCII( buf );
	  
	} 
	
        count = strlen( convert );
	if ( copy_to_user(buf, convert, count) ) {
		up (&dev->sem);
		return -EFAULT;
	}
	kfree(convert);
	up (&dev->sem);

	/* finally, awake any writers and return */
 	wake_up_interruptible(&dev->write_queue);
	printk(KERN_ALERT "READ: verlasse read mit buf=[%s], count=%d\n", buf, count);
	return count;
}

/* Wait for space for writing; caller must hold device semaphore.  On
 * error the semaphore will be released before returning. 
 */
static int scull_getwritespace(struct morse *dev, struct file *filp)
{
	while (spacefree(dev) == 0) { /* full */
		DEFINE_WAIT(wait);
		
		up(&dev->sem);
		if (filp->f_flags & O_NONBLOCK)
			return -EAGAIN;
		PDEBUG("\"%s\" writing: going to sleep\n",current->comm);
		prepare_to_wait(&dev->write_queue, &wait, TASK_INTERRUPTIBLE);
		if (spacefree(dev) == 0)
			schedule();
		finish_wait(&dev->write_queue, &wait);
		if (signal_pending(current))
			return -ERESTARTSYS; /* signal: tell the fs layer to handle it */
		if (down_interruptible(&dev->sem))
			return -ERESTARTSYS;
	}
	return 0;
}	

/* How much space is free? */
static int spacefree(struct morse *dev)
{
	if (dev->rp == dev->wp)
		return dev->buffersize - 1;
	return ((dev->rp + dev->buffersize - dev->wp) % dev->buffersize) - 1;
}

/*
 * Data management: write -> lies die Daten aus dem Anwendungsprogramm ein
 */
static ssize_t morse_write(struct file *filp, const char __user *buf, size_t count,
                loff_t *f_pos)
{
	struct morse *dev = filp->private_data;
	int result;
	
	printk(KERN_ALERT "WRITE: Ich schreibe (Typ %d): [%s]\n", dev->type, buf);

	if (down_interruptible(&dev->sem))
		return -ERESTARTSYS;

	/* Make sure there's space to write */
	result = scull_getwritespace(dev, filp);
	if (result)
		return result; /* scull_getwritespace called up(&dev->sem) */

	/* ok, space is there, accept something */
	count = min(count, (size_t)spacefree(dev));
	if (dev->wp >= dev->rp)
		count = min(count, (size_t)(dev->end_buf - dev->wp)); /* to end-of-buf */
	else /* the write pointer has wrapped, fill up to rp-1 */
		count = min(count, (size_t)(dev->rp - dev->wp - 1));
	PDEBUG("Going to accept %li bytes to %p from %p\n", (long)count, dev->wp, buf);
	if (copy_from_user(dev->wp, buf, count)) {
		up (&dev->sem);
		return -EFAULT;
	}
	dev->wp += count;
	if (dev->wp == dev->end_buf)
		dev->wp = dev->buffer; /* wrapped */
	up(&dev->sem);

	/* finally, awake any reader */
	wake_up_interruptible(&dev->read_queue);  /* blocked in read() and select() */

	PDEBUG("\"%s\" did write %li bytes\n",current->comm, (long)count);
	printk(KERN_ALERT "WRITE: fertig\n");
	return count;
}

/*
 * The file operations for the morse device
 */
struct file_operations morse_fops = {
	.owner =	THIS_MODULE,
	.llseek =	no_llseek,
	.read =		morse_read,
	.write =	morse_write,
	.open =		morse_open,
	.release =	morse_release,
};

/*
 * The cleanup function is used to handle initialization failures as well.
 * Thefore, it must be careful to work correctly even if some of the items
 * have not been initialized
 */
void morse_cleanup_module(void)
{
  
  int i = 0;
  dev_t devno = MKDEV(major, morse_minor);
  if (!morse_devices){
    return; /* nothing else to release */
  }
  
  for (i = 0; i < nopen; i++) {
    cdev_del(&morse_devices[i].cdev);
    kfree(morse_devices[i].buffer);
  }

  kfree(morse_devices);
  morse_devices = NULL; /* pedantic */
   /* cleanup_module is never called if registering failed */
   printk(KERN_ALERT "Ich bin eingeschlafen\n");
   unregister_chrdev_region(devno, nopen);
}


/*
 * Set up the char_dev structure for this device.
 */
static void morse_setup_cdev(struct morse *dev, int index)
{
   int err, devno = MKDEV(major, morse_minor + index);
    
   cdev_init(&dev->cdev, &morse_fops);
   dev->cdev.owner = THIS_MODULE;
   dev->cdev.ops = &morse_fops;
   err = cdev_add (&dev->cdev, devno, 1);
   /* Fail gracefully if need be */
   if (err)
      printk(KERN_NOTICE "Error %d adding scull%d", err, index);
}


int morse_init_module(void)
{
   int result;
   dev_t dev = 0;

/* 
 * man benoetigt 2 minor numbers(fuer morse und esrom)
 * und eine dynamisch angelegte major number
 */
   
   /* Die beiden Devices erzeugen */
   result = alloc_chrdev_region(&dev, MORSE_MINOR, 2, "morse");
   major = MAJOR(dev);
   
   if (result < 0) {
      printk(KERN_WARNING "morse: can't get major %d\n", major);
      return result;
   }
   
   /* Speicher reservieren für die beiden Devices (MORSE und ESROM) */
   morse_devices = kmalloc(2 * sizeof(struct morse), GFP_KERNEL);
   if (!morse_devices) {
      result = -ENOMEM;
      goto fail;  /* Make this more graceful */
   }
   memset(morse_devices, 0, 2 * sizeof(struct morse));
   
   /* Initialize MORSE Device */
  init_waitqueue_head(&(morse_devices[MORSE_MINOR].read_queue));
  init_waitqueue_head(&(morse_devices[MORSE_MINOR].write_queue));
  init_MUTEX(&morse_devices[MORSE_MINOR].sem);
  morse_setup_cdev(&morse_devices[MORSE_MINOR], MORSE_MINOR);
  morse_devices[MORSE_MINOR].type = MORSE_MINOR;
  
  /* Initialize ESROM Device */
  init_waitqueue_head(&(morse_devices[ESROM_MINOR].read_queue));
  init_waitqueue_head(&(morse_devices[ESROM_MINOR].write_queue));
  init_MUTEX(&morse_devices[ESROM_MINOR].sem);
  morse_setup_cdev(&morse_devices[ESROM_MINOR], ESROM_MINOR);
  morse_devices[ESROM_MINOR].type = ESROM_MINOR;

   return 0; /* succeed */

  fail:
   morse_cleanup_module();
   return result;
}

char* convertToMorse(const char* asciiStr)
{
  
    char* output=kcalloc((strlen(asciiStr)+1)*SIZE_MORSE,sizeof(char),GFP_KERNEL);//anzahl der eingegebenen chars * anzahl der maximalen Laenge eines Morsezeichens
    //calloc entspricht malloc, allerdings werden die einzelnen Elemente mit 0 vorinitialisiert
    int i=0; 
  
    printk(KERN_ALERT "ToMorse: Start, Eingabe = [%s]\n", asciiStr);
    for(i=0;i<strlen(asciiStr);i++){
        //entsprechenden Wert aus der Tabelle suchen und in akt Zwischenspeichern
        if((asciiStr[i]>='A')&&(asciiStr[i]<='Z')){
            strncat(output,table[asciiStr[i]-'A'][1],strlen(table[asciiStr[i]-'A'][1]));
            strcat(output," ");
        }else if((asciiStr[i]>='0')&&(asciiStr[i]<='9')){
            strncat(output,table[asciiStr[i]-'0'+26][1],strlen(table[asciiStr[i]-'0'+26][1]));//26 steht fuer Anzahl der Buchstaben im Alphabet ;)
            strcat(output," ");
        } else if (asciiStr[i]==' '){
            strncat(output,"   ",3);
        } // else: ignorieren
    }

    printk(KERN_ALERT "ToMorse: Ende, Ausgabe = [%s](len %d)\n", output, strlen(output));
    strncat(output,"\n",1);
    return output;
 
}

char* convertToASCII(char* morseStr)
{
    int i;
    int morseLen;
    char* morseEnd;
    int spaceCounter;
    char* ascii=kcalloc(strlen(morseStr),sizeof(char),GFP_KERNEL);
    
    printk(KERN_ALERT "ToASCII: Start, Eingabe = [%s]\n", morseStr);

    // Gesamte Eingabelaenge merken
    morseLen = strlen(morseStr);
    morseEnd = morseStr + morseLen;
    // Alle Leerzeichen durch 0 ersetzen -> Damit wird aus der Eingabe eine Liste von 
    // Endsymbol begrenzten Strings!
    for(i=0;i<morseLen;i++){
      if (morseStr[i]<=' ') morseStr[i]=0;
    }
    
    // Ganzen Satz parsen
    while(morseStr<morseEnd) {
      printk(KERN_ALERT "ToASCII: Naechste Wort ist [%s]\n", morseStr);
      // Prüfen, ob das naechste Wort der Eingabe gueltig ist
      for(i=0;i<SIZE_TABLE;i++){
		if(strcmp(morseStr,table[i][1])==0){//wenn der richtige Eintrag in der Tabelle gefunden wurde
            printk(KERN_ALERT "ToASCII: Treffer: Morse[%s] == Ascii[%s]\n", morseStr, table[i][0]);
			strncat(ascii,table[i][0],1);
			break;
		}
      }
      // Springe zum naechsten Wort
      morseStr = morseStr + strlen(morseStr) + 1;
      spaceCounter = 1;
      printk(KERN_ALERT "ToASCII: Zaehle Leerzeichen: %d\n", spaceCounter);
      while(*morseStr==0 && morseStr<morseEnd) {
		spaceCounter++;
		morseStr++;
        printk(KERN_ALERT "ToASCII: Zaehle Leerzeichen: %d\n", spaceCounter);
		if ((spaceCounter%3) == 0) {
          printk(KERN_ALERT "ToASCII: Fuege Leerzeichen in Ascii ein, Ausgabe bis jetzt [%s]\n", ascii);
		strncat(ascii," ",1);
		}
      }
    }
    strncat(ascii,"\n",1);
    printk(KERN_ALERT "ToASCII: Ende, Ausgabe = [%s](len %d)\n", ascii, strlen(ascii));
    return ascii;
}

module_init(morse_init_module);
module_exit(morse_cleanup_module);
MODULE_AUTHOR("Natalia Duske, Melanie Remmels");
MODULE_LICENSE("Dual BSD/GPL");
