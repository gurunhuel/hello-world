/* includes */

#include <vxWorks.h>
#include <ioLib.h>
#include <stdlib.h>
#include <iosLib.h>
#include <stdio.h>
#include <errnoLib.h>
#include <string.h>
#include <assert.h>
#include <semLib.h>
#include <sys/stat.h>
#include <dropbox.h>
#include <private/kernelBaseLibP.h>
#include <dirent.h>
#include <taskLib.h>
#include <memStream.h>
#include <private/taskLibP.h>

#ifdef  DRBFS_DEBUG
#include <stdio.h>
#include <logLib.h>
#include <intLib.h>
#define DRBFS_DEBUG_LOG(msg,args...)                                     \
    {                                                                    \
    if (INT_CONTEXT ())                                                  \
        {                                                                \
        logMsg ("%s (%s:%d) :", (int) __FUNCTION__,                      \
                                (int) __FILE__, __LINE__, 0, 0, 0);      \
        logMsg (msg, 0, 0, 0, 0, 0, 0);                                  \
        }                                                                \
    else                                                                 \
        {                                                                \
        (void)printf ("%s (%s:%d) :", __FUNCTION__, __FILE__, __LINE__); \
        (void)printf (msg, ## args);                                     \
        }                                                                \
    }
#else
#define DRBFS_DEBUG_LOG(msg,args...)
#endif  /* DEBUG */

#define MAX_MNT_PER_DEV 40
#define DEV_ID          "DRBFS_DEV"
#define MAX_DEVID_SIZE  (sizeof(DEV_ID) + 10)
#define MAX_MNTID_SIZE  (MAX_DEVID_SIZE + 4 + 2 + 1)
#define MNT_ID          "%s_MNT%d"
#define DRBFS_MUTEXOPT  (SEM_Q_PRIORITY | SEM_DELETE_SAFE | SEM_INVERSION_SAFE)
#define ROOT_DIR 	    "root"
#define FAKE_DIR_FD 	"FAKEDIRFD"
#define MAX_RW_SIZE     (1024 * 128)
#define ALT_BUF_SIZE    (1024 * 8)

#define DRBFS_DEV_GET(devId) drbFsDevGet (devId)
#define IS_MNT_FREE(mnt)     (mnt.id[0] == 0)
#define IS_MODE_CREAT(mode)  ((mode & O_CREAT) == O_CREAT)
#define IS_MODE_WRITE(mode)  ((mode & O_WRONLY) == O_WRONLY || (mode & O_RDWR) == O_RDWR)
#define IS_AUTO_MNT(mnt)     (strncmp (mnt.name, ROOT_DIR, strlen (ROOT_DIR)) == 0)

#define IS_WIN_AUTO_MNT(mnt) 			             		\
	((strlen (mnt.name) == (strlen (ROOT_DIR) + 2)) &&  	\
	IS_AUTO_MNT(mnt) && 		                			\
	(mnt.name[strlen (ROOT_DIR)] == '/'))

/* IS_DEVICE_PATH is TRUE if path = "" or "/" or "/./" */
#define IS_DEVICE_PATH(path) 					                    	\
    ((strlen (path) == 0) || 				                    		\
     ((strlen (path) == 1) && (path[0] == '/')) ||	            		\
     ((strlen (path) == 3) && (path[0] == '/') && (path[1] == '.') && 	\
      (path[2] == '/')))

/* IS_ROOT_PATH is TRUE if path = "/root" or "/root/" or "/root/./ */
#define IS_ROOT_PATH(path)				                                	\
    (((strlen (path) == (strlen (ROOT_DIR) + 1)) ||	                     	\
     ((strlen (path) == (strlen (ROOT_DIR) + 2))) ||                   		\
     ((strlen (path) == (strlen (ROOT_DIR) + 4)) && (path [6] == '.'))) &&  \
     (path [0] == '/') &&				                                 	\
     (strncmp (&path[1], ROOT_DIR, strlen (ROOT_DIR)) == 0))

#define IS_FAKE_DIR_FD(pCharDesc)                                   \
    (pCharDesc->isDirFd && (strncmp (pCharDesc->fd, FAKE_DIR_FD, \
                                     strlen(FAKE_DIR_FD)) ==0))

#define FREE_MNT(mnt) free ((void *) mnt.name); 	                 \
                      free ((void *) mnt.rootPath); 			     \
                      free ((void *) mnt.targetPath); 		         \
                      if (mnt.clientData != NULL) 			         \
	                      free ((void *) mnt.clientData);		     \
                      memset (&mnt, 0, sizeof (DRBFS_MOUNT_POINT));

typedef struct 
    {
	char * name;
	char * rootPath;
	char * targetPath;
	char   id[MAX_MNTID_SIZE];
    char   devId[MAX_DEVID_SIZE];
    BOOL   isReadOnly;
    void * clientData;
    } DRBFS_MOUNT_POINT;

typedef struct drbfs_dev 
    {
	DEV_HDR           devHdr;
	char *            name;
	DRBFS_MOUNT_POINT mntTbl[MAX_MNT_PER_DEV];
	unsigned int      nbMnt;
    char              id[MAX_DEVID_SIZE];
    char *            clientData;
    int               drbFsChanNum;
    } DRBFS_DEV;

typedef struct drbfs_dev_list 
    {
	struct drbfs_dev_list * pNext;
	DRBFS_DEV               dev;
    } DRBFS_DEV_LIST;

typedef struct FileNode 
    {
    char *            file_name;
    struct FileNode * next;
    } FileNode;

typedef struct
    {
    int                 channel;
    int			        openmode;
    char *		        pathname;
    char *              fd;
    BOOL		        isDirFd;
    struct FileNode * 	fileList;
    struct FileNode * 	curNode;
    memStream           stream;
    off_t		        offset;
    char * 		        altBuf;
    SEM_ID		        syncSemId;
    SEM_ID 		        semId;
    DRBFS_DEV * 	    pDev;
    } DRBFS_FILE_DESC;

static int              drbFsDrvNum = 0;
static int              drbFsDevCnt = 1;
static DRBFS_DEV_LIST * pDrbFsDev;
static int              drbFsAltBufSize = ALT_BUF_SIZE;
static int              drbFsMaxRWBufSize = MAX_RW_SIZE;

static DRBFS_FILE_DESC * drbFsCreate (DRBFS_DEV *pDev, char *name, int mode);
static DRBFS_FILE_DESC * drbFsOpen (DRBFS_DEV *pDev, char * name, int mode, int perm);
static int     	         drbFsDelete (DRBFS_DEV *pDev, char * name);
static STATUS  	         drbFsClose (DRBFS_FILE_DESC *pDrbFsFd);
static ssize_t 	         drbFsRead  (DRBFS_FILE_DESC *pNetFd, char *buf, size_t maxBytes);
static ssize_t 	         drbFsWrite (DRBFS_FILE_DESC *pNetFd, char *buf, size_t maxBytes);
static STATUS	         drbFsIoctl (DRBFS_FILE_DESC *pDrbFsFd, int request, _Vx_ioctl_arg_t arg);

STATUS drbFsLibInit (void) 
    {
	if (drbFsDrvNum > 0) return OK;
	
	if ((pDrbFsDev = (DRBFS_DEV_LIST *) malloc (drbFsDevCnt * sizeof (DRBFS_DEV_LIST))) == NULL)
		return ERROR;
	memset (pDrbFsDev, 0, sizeof (DRBFS_DEV_LIST));
	
	drbFsDrvNum = iosDrvInstall ((DRV_CREATE_PTR) drbFsCreate, (DRV_REMOVE_PTR) drbFsDelete, (DRV_OPEN_PTR) drbFsOpen,
		                         (DRV_CLOSE_PTR) drbFsClose, (DRV_READ_PTR) drbFsRead, (DRV_WRITE_PTR) drbFsWrite, 
		                         (DRV_IOCTL_PTR) drbFsIoctl);

    if (drbFsDrvNum <= 0)
        return (ERROR);

    return (OK);
    }

static DRBFS_DEV * drbFsDevGet
    (
    char * devId
    )
    {
    DRBFS_DEV_LIST * pDevList = pDrbFsDev;

    if (devId == NULL)
    	return NULL;
    
    while (pDevList != NULL)
        {
        if (strncmp (pDevList->dev.id, devId, strlen (devId)) == 0)
            return (&pDevList->dev);
        pDevList = pDevList->pNext;
        }
    return (NULL);
    }

STATUS drbFsMountPointAdd 
    (
    char *	devId,	    /* device ID */
    char * 	name,	    /* mount point name */
    char * 	rootPath,   /* root path of the host file system */
    BOOL 	readOnly,   /* TRUE to restrict to READ only mode */
    void *	clientData,
    char ** mntId
    )
    {
    DRBFS_MOUNT_POINT * newMnt = NULL;
    DRBFS_DEV * pDev = &pDrbFsDev->dev; /* for now only support one dev */
    unsigned int ix;
    unsigned int id = 0;

    DRBFS_DEBUG_LOG ("Adding the mount point id:%s, name: %s, root: %s\n", devId, name, rootPath);

    if ((pDev = DRBFS_DEV_GET (devId)) == NULL)
	    {
	    (void) errnoSet (ENODEV);
	    return (ERROR);
	    }	

    if (pDev->nbMnt >= MAX_MNT_PER_DEV)
	    {
	    (void) errnoSet (ENOSPC);
	    return (ERROR);
	    }

    /* check parameters */
    
    if ((name == NULL) || (rootPath == NULL))
	    {
	    (void) errnoSet (EINVAL);
	    return (ERROR);
	    }

    /* 
     * check that name is not already used and find a free place into
     * mount point table.
     */

    for (ix = 0; ix < MAX_MNT_PER_DEV; ix++)
        {
        if (IS_MNT_FREE (pDev->mntTbl[ix]))
	        {
  	        if (newMnt == NULL)
		        {
		        newMnt = (DRBFS_MOUNT_POINT *) &pDev->mntTbl[ix];
		        id = ix;
		        }
	        }
   	     else 
	        {
	        if ((strlen (name) == strlen (pDev->mntTbl[ix].name)) &&
	            (strncmp (name, pDev->mntTbl[ix].name,
		                  strlen (pDev->mntTbl[ix].name) == 0)))
	            {
		        /* mount point name already used */

	            (void) errnoSet (EINVAL);
	            return (ERROR);
	            }	
	        }
	    }

        /* 
         * newMnt should not be null. If 'pDev->nbMnt < MAX_MNT_PER_DEV)',
         * a free location inside pDev->mntTbl must exists.
         */
	
    assert (newMnt != NULL);

    if (newMnt == NULL)
     	goto mountError;

    if ((newMnt->name = strdup (name)) == NULL)
      	{
       	(void) errnoSet (ENOMEM);
       	goto mountError;
       	}

    if ((newMnt->rootPath = strdup (rootPath)) == NULL)
       	{
       	(void) errnoSet (ENOMEM);
       	goto mountError;
       	}

    if (memcpy (newMnt->devId, devId, MAX_DEVID_SIZE) == NULL)
       	{
       	(void) errnoSet (ENOMEM);
       	goto mountError;
       	}

    if (clientData != NULL)
       	newMnt->clientData = clientData;

    if (snprintf (newMnt->id, MAX_MNTID_SIZE, MNT_ID, newMnt->devId, id) == 0)
       	{
       	(void) errnoSet (ENOMEM);
        goto mountError;
        }

    if ((newMnt->targetPath = malloc (strlen(newMnt->name) + strlen (pDev->name) + 3)) == NULL)
        {
        (void) errnoSet (ENOMEM);
        goto mountError;
        }
    memset (newMnt->targetPath, 0, strlen(newMnt->name) + strlen (pDev->name) + 3);
    
    if (snprintf (newMnt->targetPath, 
                  strlen (pDev->name) + strlen (newMnt->name) + 3, "%s/%s/", pDev->name,
                  newMnt->name) == 0)
        {
        (void) errnoSet (ENOMEM);
        goto mountError;
        }

    newMnt->isReadOnly = readOnly;

    if (mntId != NULL)
   	    *mntId = newMnt->id;
    	
    return (OK);

mountError:
    if (newMnt != NULL)
    	{
	    if (newMnt->name != NULL)
    	    free ((void *) newMnt->name);
	    if (newMnt->rootPath != NULL)
	        free ((void *) newMnt->rootPath);
	    if (newMnt->targetPath != NULL)
	        free ((void *) newMnt->targetPath);
	    }
    return (ERROR);
    }

STATUS drbFsMountPointDelete
    (
    char * devId,
    char * mntId
    )
    {
    unsigned int ix;
    BOOL mntFound = FALSE;
    DRBFS_DEV * pDev = NULL;

    DRBFS_DEBUG_LOG ("Deleting the mount point %s\n", mntId);

    if ((pDev = DRBFS_DEV_GET (devId)) == NULL)
	    {
	    (void) errnoSet  (ENODEV);
	    return (ERROR);
	    }	

    /* search the mount point entry corresponding to 'mntId' */

    for (ix = 0; ix < MAX_MNT_PER_DEV; ix++)
    	{
	    if (!IS_MNT_FREE (pDev->mntTbl[ix]) && 
	        (strncmp (mntId, pDev->mntTbl[ix].id, MAX_MNTID_SIZE) == 0))
	        {
    	    FREE_MNT (pDev->mntTbl[ix]);
	        mntFound = TRUE;
 	        break;
	        }
	    }

    if (!mntFound)
        {
	    (void) errnoSet (EINVAL);
        return (ERROR);
	    }

    pDev->nbMnt --;
	
    return (OK);
    }

STATUS drbFsDevInit
    (
    char *	devName,
    char *	clientData,	/* client specific data */
    char **	devId
    )
    {
    int ix;
    DRBFS_DEV * pDev;
    DRBFS_DEV_LIST * pDevList;
    DRBFS_DEV_LIST * pLastDev = NULL;

    DRBFS_DEBUG_LOG ("Associate the %s device with the drbFs library\n", devName);

    if (drbFsDrvNum <= 0)
        return (ERROR);

    if (devName == NULL)
	    {
	    (void) errnoSet (EINVAL);
	    return (ERROR);
	    }

    /* find a free slot */

    pDevList = pDrbFsDev;
    ix = 0;
    while (pDevList != NULL)
        {
        pDev = &pDevList->dev;
        if (pDev->name == NULL) break;
        pLastDev = pDevList;
        pDevList = pDevList->pNext;
        ix++;
        }
    if (pDevList == NULL) /* available ones are busy : allocate a new one */
    	{ 
        pDevList = (DRBFS_DEV_LIST *) malloc (sizeof (DRBFS_DEV_LIST));
        if (pDevList == NULL)
            {
            (void) errnoSet (ENOMEM);
            return (ERROR);
            }
        memset (pDevList, 0, sizeof (DRBFS_DEV_LIST));
            
        pDev = &pDevList->dev;
        if (pLastDev != NULL)
            pLastDev->pNext = pDevList;
        else
            pDrbFsDev = pDevList;
        drbFsDevCnt ++; 
        }	
    else
        memset (pDev, 0, sizeof (DRBFS_DEV));

    /* Add device to system device table */

    if (iosDevAdd (&pDev->devHdr, devName, drbFsDrvNum) != OK)
        {
        if (errno == S_iosLib_DUPLICATE_DEVICE_NAME)
            (void)errnoSet (EEXIST);
        return (ERROR);
        }

    /* initialize the drbfs device */

    if (clientData != NULL)
	    {
    	if ((pDev->clientData = strdup(clientData)) == NULL)
	        {
 	        (void) errnoSet (ENOMEM);
            (void) iosDevDelete (&pDev->devHdr);
	        return (ERROR);
	        }
    	}

    if  ((pDev->name = strdup(devName)) == NULL)
	    {	
	    (void) errnoSet (ENOMEM);
        (void) iosDevDelete (&pDev->devHdr);
	    if (pDev->clientData != NULL)
	        free ((void *) pDev->clientData);
	    return ERROR;
	    }

    if (snprintf (pDev->id, MAX_DEVID_SIZE, "%s%d", DEV_ID, ix) >= MAX_DEVID_SIZE)
	    {
	    (void) errnoSet (ENOMEM);
        (void) iosDevDelete (&pDev->devHdr);
	    free ((void *) pDev->name);
	    if (pDev->clientData != NULL)
	        free ((void *) pDev->clientData);
	    return ERROR;
	    }

    if (devId != NULL)
        *devId = pDev->id;

    return (OK);
    }

STATUS drbFsDevDelete
    (
    char * devId
    )
    {
    unsigned int ix;
    DRBFS_DEV * pDev = NULL;
    STATUS status;
    
    DRBFS_DEBUG_LOG ("Deleting the drbfs device\n");
    
    if ((pDev = DRBFS_DEV_GET (devId)) == NULL)
	    {
	    (void) errnoSet (ENODEV);
	    return (ERROR);
	    }	

    /* free all the associated mount points */

    for (ix = 0; ix < MAX_MNT_PER_DEV; ix++)
        {
        if (IS_MNT_FREE (pDev->mntTbl[ix])) 
	    continue;

        (void) drbFsMountPointDelete (devId, pDev->mntTbl[ix].id);
	    }

    if (pDev->clientData !=  NULL)
	    free ((void *) pDev->clientData);

    free (pDev->name);

    /* delete the device from the I/O system */

    status = iosDevDelete (&pDev->devHdr);

    memset (pDev, 0, sizeof (DRBFS_DEV));

    return (status);
    }

static DRBFS_FILE_DESC * drbFsCreate 
    (
    DRBFS_DEV * pDev, 
	char *      name, 
	int         mode
	) 
    {
	DRBFS_DEBUG_LOG ("Dropbox FS File Creation, name: %s, mode: %d\n", name, mode);
	return drbFsOpen (pDev, name, mode | O_CREAT | O_TRUNC, 0666);
    }

static BOOL isMounted 
    (
    DRBFS_DEV * pDev
    )
    {
    if (pDev == NULL || pDev->name == NULL)
        return FALSE;

    return TRUE;
    }

static char * toHostPath 
    (
    DRBFS_DEV * pDev,
    char * 		targetPath,	/* target path to translate */
    BOOL * 		isReadOnly
    )
    {
    char *			    hostPath = NULL;
    size_t	 		    lenHostPath = 0;
    unsigned int	    ix;
    DRBFS_MOUNT_POINT *	pMnt = NULL;
    
    DRBFS_DEBUG_LOG ("Translate target path %s.\n", targetPath);

    /*
     * validate that target path uses format : 
     *   - /<mount_point_name>/...
     */

    if (targetPath[0] != '/')
	    {
        DRBFS_DEBUG_LOG ("Target path first character is not '/'.\n");
	    (void) errnoSet (ENOENT);
 	    return NULL;
	    }

    /* determine the mount point associated with the target path */

    for (ix = 0; ix < MAX_MNT_PER_DEV; ix++)
        {
	    size_t lenMntName;
	    char * mntName;

	    if (IS_MNT_FREE (pDev->mntTbl[ix]))
	        continue;

	    lenMntName = strlen (pDev->mntTbl[ix].name);
	    mntName = pDev->mntTbl[ix].name;

	    if ((strncmp (mntName, &targetPath[1], lenMntName) == 0) &&
	        (lenMntName == strlen (&targetPath[1]) ||
	        (targetPath[lenMntName + 1] == '/')))
	        {
	        pMnt = &pDev->mntTbl[ix];
 	        break;
	        }
	    }

    if (pMnt == NULL)
	    {
	    DRBFS_DEBUG_LOG ("Unable to determine the associated mount point.\n");
	    (void) errnoSet (ENOENT);
 	    return NULL;
	    }

    /* replace "mount_point_name" with "root path" to get the host path */

    lenHostPath = strlen (targetPath) + strlen (pMnt->rootPath) - 
		          strlen (pMnt->name) + 1;
	
    if ((hostPath = (char *) malloc (lenHostPath)) == NULL)
	    {
        (void) errnoSet (ENOMEM);
	    return NULL;
	    }
    memset (hostPath, 0, lenHostPath);
    
    if (strncpy (hostPath, pMnt->rootPath, strlen (pMnt->rootPath)) == NULL)
	    {
	    free ((void *) hostPath);
        (void) errnoSet (ENOMEM);
	    return NULL;
	    }

    if (strncat (hostPath, &targetPath[strlen (pMnt->name) + 1], 
	    strlen (&targetPath[strlen (pMnt->name) + 1])) == NULL)
	    {
	    free ((void *) hostPath);
        (void) errnoSet (ENOMEM);
	    return NULL;
        }

    /* 
     * if host path last character is a '/', remove it
     * as it fails with value-add running on Windows.
     */

 
    if (hostPath [strlen (hostPath) -1] == '/')
	    hostPath [strlen (hostPath) -1] = 0;

    *isReadOnly = pMnt->isReadOnly;

    DRBFS_DEBUG_LOG ("Translated host path = %s.\n", hostPath);
    return (hostPath);
    }

static drbClient * drbFsClientGet
    (
    DRBFS_FILE_DESC * pDrbFsFd
	)
    {
    for (int ix = 0; ix < MAX_MNT_PER_DEV; ix++)
    	if (pDrbFsFd->pDev->mntTbl[ix].name != NULL &&
    		strncmp (&pDrbFsFd->pathname[1], pDrbFsFd->pDev->mntTbl[ix].name, strlen (pDrbFsFd->pDev->mntTbl[ix].name)) == 0)
    		return pDrbFsFd->pDev->mntTbl[ix].clientData;
    return NULL;
    }

static STATUS drbFsStat 
    (
    DRBFS_FILE_DESC * pDrbFsFd,  /* file handle */
    struct stat * 	  pStat      /* structure to fill with data */
    )
    {
    int           statStatus = 0;
    struct stat   fdStat;
    int	          posixErrno = 0;
    void *        output = NULL;
    int           err;
    drbMetadata * meta;
    drbClient *   cli;
    
    if (!isMounted (pDrbFsFd->pDev))
	    {
	    (void) errnoSet (EAGAIN); 
  	    return (ERROR);	
	    }

    if (semTake (pDrbFsFd->semId, WAIT_FOREVER) == ERROR)
    	return (ERROR);

    memset (&fdStat, 0, sizeof (struct stat));
    if (pDrbFsFd->fd != NULL)
    	{
    	DRBFS_DEBUG_LOG ("Request file stat info, fd: %s\n", pDrbFsFd->fd);
    	
        if ((cli = drbFsClientGet (pDrbFsFd)) == NULL)
        	return ERROR;
        err = drbGetMetadata(cli, &output,
                             DRBOPT_PATH, pDrbFsFd->fd,
                             DRBOPT_LIST, true,
                             //                     DRBOPT_FILE_LIMIT, 100,
                             DRBOPT_END);
        if (err != DRBERR_OK) 
            {
        	DRBFS_DEBUG_LOG ("Metadata error (%d): %s\n", err, (char*)output);
            free(output);
        	(void) semGive (pDrbFsFd->semId);
            return (ERROR);            
            } 
        else 
            {
            meta = (drbMetadata *) output;
            fdStat.st_size = *meta->bytes;
            if (meta->contents)
            	fdStat.st_mode = S_IFDIR;
            // displayMetadata(meta, "Metadata");
            drbDestroyMetadata(meta, true);
            }
    	}
    else if (IS_DEVICE_PATH (pDrbFsFd->pathname) || IS_ROOT_PATH (pDrbFsFd->pathname))
	    {
	    DRBFS_DEBUG_LOG ("Request file stat info for dropbox device\n");
	
	    fdStat.st_mode = S_IFDIR;
	    }
    else
	    {
        char * hostPath;
	    BOOL   isRdMnt;
	    
        DRBFS_DEBUG_LOG ("Request file stat info, name = %s\n", 
			  pDrbFsFd->pathname);

        if ((hostPath = toHostPath (pDrbFsFd->pDev, pDrbFsFd->pathname, 
	                                &isRdMnt)) == NULL)
	        {
	        (void) semGive (pDrbFsFd->semId);
	        return (ERROR); 
	        }
 
        if ((pDrbFsFd->pDev == NULL) || ((cli = drbFsClientGet (pDrbFsFd)) == NULL))
        	{
        	free ((void *) hostPath);
        	(void) semGive (pDrbFsFd->semId);
            return (ERROR);
        	}
        
        err = drbGetMetadata(cli, &output,
                             DRBOPT_PATH, hostPath,
                             DRBOPT_LIST, true,
                             //                     DRBOPT_FILE_LIMIT, 100,
                             DRBOPT_END);
        free ((void *) hostPath);
        
        if (err != DRBERR_OK) 
            {
        	DRBFS_DEBUG_LOG ("Metadata error (%d): %s\n", err, (char*)output);
            free(output);
        	(void) semGive (pDrbFsFd->semId);
            return (ERROR);            
            } 
        else 
            {
            meta = (drbMetadata *) output;
            fdStat.st_size = *meta->bytes;
            if (meta->contents)
            	fdStat.st_mode = S_IFDIR;
            // displayMetadata(meta, "Metadata");
            drbDestroyMetadata(meta, true);
            }
	    }

    if (statStatus == 0)
	    {
	    pStat->st_size = fdStat.st_size;
	    pStat->st_uid = fdStat.st_uid;
	    pStat->st_gid = fdStat.st_gid;
    	pStat->st_mode = fdStat.st_mode;
	    pStat->st_atime = fdStat.st_atime;
	    pStat->st_mtime = fdStat.st_mtime;
	    (void) semGive (pDrbFsFd->semId);
		DRBFS_DEBUG_LOG ("Stat - size: %i, uid: %i, gid: %i, mode: %i, atime: %i, mtime: %i\n",
						 (int) pStat->st_size, pStat->st_uid, pStat->st_gid,
						 pStat->st_mode, (int) pStat->st_atime, (int) pStat->st_mtime);
	    return (OK);
	    }
    
    if (posixErrno > 0)
        (void) errnoSet (posixErrno);

    DRBFS_DEBUG_LOG ("Error requesting stat info = %s\n (errno= 0x%x)\n",
	                 pDrbFsFd->pathname, posixErrno);
  
    (void) semGive (pDrbFsFd->semId);
    return (ERROR);
    }	

static DRBFS_FILE_DESC * drbFsOpen (DRBFS_DEV *pDev, char * name, int mode, int perm) 
    {
    DRBFS_FILE_DESC * pDrbFsFd;		/* opened file handle */
    int			      status = 0;
    int			      posixErrno = 0;
    BOOL		      writeAccess = FALSE;

	DRBFS_DEBUG_LOG ("Dropbox FS Opening - pDev: 0x%x, name: %s, mode: %i, perm: %i\n",
			         (unsigned int) pDev, name, mode, perm);

    if (!isMounted (pDev))
	{
	(void) errnoSet (EAGAIN);
	return ((DRBFS_FILE_DESC *) ERROR);
	}

    /* Create a channel descriptor */

    pDrbFsFd = (DRBFS_FILE_DESC *) malloc (sizeof (DRBFS_FILE_DESC));
    
    if (pDrbFsFd == NULL)
	    {
	    (void) errnoSet (ENOMEM);
	    return ((DRBFS_FILE_DESC *) ERROR); 
	    }
    memset (pDrbFsFd, 0, sizeof (DRBFS_FILE_DESC));

    /* Store the filename for a latter use by FIOUNLINK ioctl() */

    if ((pDrbFsFd->pathname = strdup (name))== NULL)
	    {
	    (void) errnoSet (ENOMEM);
	    goto error;
	    }

    /* initialize the channel descriptor */

    pDrbFsFd->channel = pDev->drbFsChanNum++;
    pDrbFsFd->openmode = mode;
    pDrbFsFd->pDev = pDev;

    /*  
     * initialize binary semaphore to make FileSystem proxy requests
     * synchronous.
     */

    if ((pDrbFsFd->syncSemId = semBCreate (SEM_Q_FIFO, SEM_EMPTY)) == NULL)
	    goto error;

#ifdef	_WRS_CONFIG_OBJECT_OWNERSHIP
    if (objOwnerSet (pDrbFsFd->syncSemId, kernelIdGet()) == ERROR)
	    goto error;
#endif	/* _WRS_CONFIG_OBJECT_OWNERSHIP */

    /* initialize mutex semaphore to protect drbFs vitual I/O channel. */

    if ((pDrbFsFd->semId = semMCreate (DRBFS_MUTEXOPT)) == NULL)
	    goto error;

#ifdef	_WRS_CONFIG_OBJECT_OWNERSHIP
    if (objOwnerSet (pDrbFsFd->semId, kernelIdGet()) == ERROR)
	    goto error;
#endif	/* _WRS_CONFIG_OBJECT_OWNERSHIP */

    if (!IS_MODE_CREAT (pDrbFsFd->openmode))
    	{
	    struct stat resStat;
	    
	    if (IS_DEVICE_PATH (pDrbFsFd->pathname) || IS_ROOT_PATH (pDrbFsFd->pathname))
	        {
	        DRBFS_DEBUG_LOG ("Path is detected as a device name\n");
	        return pDrbFsFd;
	        }
	
        memset (&resStat, 0, sizeof (struct stat));

	    status = drbFsStat (pDrbFsFd, &resStat);

	    if ((status == 0) && 
	        (S_ISDIR (resStat.st_mode) || S_ISFIFO (resStat.st_mode)))
	        {
	        /*
	         * If this is a directory or a FIFO (may block) :
	         * do not open it. 
    	     * If it is a FIFO do not try to open as it may block.
	         * If it is directory, it will be opened later by
	         * drbFsDirRead() API.
	         */

	        DRBFS_DEBUG_LOG ("Path is detected as a directory\n");
	        return pDrbFsFd;
	        }
    	}	
	
    if (status == 0)
    	{
        char * hostPath = NULL;
    	BOOL isRdMnt = FALSE;

	    /* translate target to host path before to call filesystem proxy */

	    if ((hostPath = toHostPath (pDev, pDrbFsFd->pathname, &isRdMnt)) == NULL)
	        goto error;

        /* If mount point is restricted to READ access, check the permission */

        if (writeAccess && isRdMnt)
	        {	    
 	        free ((void*) hostPath);
	        goto error;
	        }

        if ((pDrbFsFd->fd = strdup (hostPath))== NULL)
    	    {
    	    (void) errnoSet (ENOMEM);
    	    goto error;
    	    }
        
        if (IS_MODE_WRITE (pDrbFsFd->openmode))
        	memStreamInit(&pDrbFsFd->stream);
        
    	free ((void *) hostPath);    
	    }

    if (status == 0)
	    {
	    DRBFS_DEBUG_LOG ("Virtual I/O channel for %s opened successfully.\n", name);
	    return pDrbFsFd;
	    }

error:
    /* Set errno if TCF errno can be converted to POSIX errno */

    if ((status != 0 ) && (posixErrno > 0))
	    (void) errnoSet (posixErrno);

    DRBFS_DEBUG_LOG ("Can't open virtual I/O channel for %s (errno %x)\n", name, posixErrno);

    if (pDrbFsFd->fd != NULL)
    	free ((void *) pDrbFsFd->fd);
    
    if (pDrbFsFd->pathname != NULL)
        free ((void*) pDrbFsFd->pathname);

    if (pDrbFsFd->syncSemId != SEM_ID_NULL)
    	(void) semDelete (pDrbFsFd->syncSemId);

    if (pDrbFsFd->semId != SEM_ID_NULL)
    	(void) semDelete (pDrbFsFd->semId);

    free ((void *) pDrbFsFd);
    return ((DRBFS_FILE_DESC *) ERROR);
    }

static int drbFsDelete
    (
    DRBFS_DEV *	pDev,		/* I/O system device entry */
    char *		name		/* name of file to destroy */
    )
    {
    int			      status = 0;
    DRBFS_FILE_DESC * pDrbFsFd;	 /* file handle */
    int 		      posixErrno = 0;
    char *		      hostPath = NULL;
    BOOL		      isRdMnt;
    void *            output = NULL;
    int               err;
    drbClient *       cli;
    
	DRBFS_DEBUG_LOG ("Dropbox FS Deletion - pDev: 0x%x, name: %s\n",
			         (unsigned int) pDev, name);

    pDrbFsFd = (DRBFS_FILE_DESC *) malloc (sizeof (DRBFS_FILE_DESC));
    if (pDrbFsFd == NULL)
	    {
	    (void) errnoSet (ENOMEM);
	    return ERROR;
	    }
    memset (pDrbFsFd, 0, sizeof (DRBFS_FILE_DESC));

    /* The file pathname is stored after the channel structure */

    if ((pDrbFsFd->pathname = strdup (name)) == NULL)
        {
    	free ((void *) pDrbFsFd);
	    return (ERROR);
	    }

    /* Translate target path to host path */

    if ((hostPath = toHostPath (pDev, pDrbFsFd->pathname, &isRdMnt)) == NULL)
	    {
    	free (pDrbFsFd->pathname);
    	free ((void *) pDrbFsFd);
	    return (ERROR);
	    }

    if (isRdMnt)
	    {
	    /* The corresponding mount point is restricted to READ access */
	
    	(void) errnoSet (EPERM);
    	free (pDrbFsFd->pathname);
        free ((void *) hostPath);
	    free ((void *) pDrbFsFd);
	    return (ERROR);
	    }
    /* Initialize binary semaphore for FS proxy synchro */

    if ((pDrbFsFd->syncSemId = semBCreate (SEM_Q_FIFO, SEM_EMPTY)) == NULL)
	    {
    	free (pDrbFsFd->pathname);
        free ((void *) hostPath);
        free ((void *) pDrbFsFd);
	    return (ERROR);
	    }

#ifdef	_WRS_CONFIG_OBJECT_OWNERSHIP
    if (objOwnerSet (pDrbFsFd->syncSemId, kernelIdGet()) == ERROR)
	    {
	    (void) semDelete (pDrbFsFd->syncSemId);
    	free (pDrbFsFd->pathname);
        free ((void *) hostPath);
        free ((void *) pDrbFsFd);
	    return (ERROR);
	    }
#endif	/* _WRS_CONFIG_OBJECT_OWNERSHIP */

    pDrbFsFd->pDev = pDev;
    if ((cli = drbFsClientGet (pDrbFsFd)) == NULL)
    	status = ERROR;
    else
        err = drbDelete (cli, &output,
                         DRBOPT_PATH, hostPath,
                         DRBOPT_END);

    if (err != 0)
	    {
        status = ERROR;

	    /* Set errno if TCF errno can be converted to POSIX errno */

	    if (posixErrno > 0)
	        (void) errnoSet (posixErrno);

        DRBFS_DEBUG_LOG ("Error deleting I/O channel, name = %s, (errno 0x%x)\n",
	                     name, posixErrno);
	    }

    /* discard the temporary channel */

    (void) semDelete (pDrbFsFd->syncSemId);
	free (pDrbFsFd->pathname);
    free ((void *) pDrbFsFd);
    free ((void *) hostPath);

    return status;
    }

static STATUS drbFsClose
    (
    DRBFS_FILE_DESC *	pDrbFsFd
    )
    {
    STATUS	status = OK;
    int 	closeStatus = 0;
    int 	posixErrno = 0;

	DRBFS_DEBUG_LOG ("Dropbox FS Closing - name: %s\n", pDrbFsFd->pathname);

    if (!isMounted (pDrbFsFd->pDev))
	    {
	    (void) errnoSet (EAGAIN); 
  	    return (ERROR);	
	    }

    if (semTake (pDrbFsFd->semId, WAIT_FOREVER) == ERROR)
    	return (ERROR);

    /* Call the Close API of the filesystem proxy. */

    if ((pDrbFsFd->fd != NULL) && !IS_FAKE_DIR_FD (pDrbFsFd) && IS_MODE_WRITE (pDrbFsFd->openmode))
        {
    	drbClient * cli;
    	
    	/* writing a null size file fails, therefore writing a file containing one space (it should solve most issues) */
    	if (pDrbFsFd->stream.size == 0)
    		{
    		pDrbFsFd->stream.data = " ";
    		pDrbFsFd->stream.size = 1;
    		}
    	
        if ((cli = drbFsClientGet (pDrbFsFd)) == NULL)
        	status = ERROR;
        else
            {
        	pDrbFsFd->stream.cursor = 0;
            closeStatus = drbPutFile(cli, NULL,
                                     DRBOPT_PATH, pDrbFsFd->fd,
                                     DRBOPT_IO_DATA, &pDrbFsFd->stream,
                                     DRBOPT_IO_FUNC, memStreamRead,
                                     DRBOPT_END);
            DRBFS_DEBUG_LOG ("File upload: %s\n", closeStatus ? "Failed" : "Successful");
            }
        }

    if (closeStatus == 0)
	    {
    	/* free the files list if channel corresponds to a directory */

    	if (pDrbFsFd->isDirFd)
	        {
	        FileNode * current_file = pDrbFsFd->fileList;
	    
	        while (current_file != NULL)
	    	    {
	    	    FileNode * next_file = current_file->next;
	    	
	    	    free ((void *) current_file->file_name);
	    	    free ((void *) current_file);
	    	    current_file = next_file;
	    	    }
	        }	

        free ((void *) pDrbFsFd->fd);
        free ((void *) pDrbFsFd->pathname);

	    /* free the alternate read/write buffer if needed */

        if (pDrbFsFd->altBuf != NULL)
	        free ((void *) pDrbFsFd->altBuf);

    	(void) semDelete (pDrbFsFd->syncSemId);
    	(void) semDelete (pDrbFsFd->semId);
    	free ((void *) pDrbFsFd);
	    }
    else
	    {
	    status = ERROR;

	    /* Set errno if TCF errno can be converted to POSIX errno */

	    if (posixErrno > 0)
	        (void) errnoSet (posixErrno);

        DRBFS_DEBUG_LOG ("Error when closing virtual I/O channel, name = %s\n",
     	                 pDrbFsFd->pathname);

	    (void) semGive (pDrbFsFd->semId);
 	    }
    return status;
    }

static ssize_t drbFsRead
    (
    DRBFS_FILE_DESC *	pDrbFsFd,
    char *		        buf,
    size_t		        maxBytes
    )
    {
    ssize_t	    bytesRead = 0;		/* Number of bytes handled */
    int 	    readStatus = 0; 	/* status of read request */
    BOOL 	    isRtpRequest;		/* Is current task a RTP task ? */
    size_t	    oneShotMaxSize;		/* Maximum size for a single request */
    size_t	    oneShotReadSize;	/* size for a read request */
    char *	    oneShotReadBuf;		/* buffer to put read data */
    ssize_t 	oneShotReadCnt;		/* read data size */
    WIND_TCB *	pTcb;

	DRBFS_DEBUG_LOG ("Dropbox FS Reading - name: %s, buf: %s, maxBytes: %i\n",
			         pDrbFsFd->pathname, buf, maxBytes);

    if (!isMounted (pDrbFsFd->pDev))
	    {
	    (void) errnoSet (EAGAIN); 
  	    return (ERROR);	
	    }

    if (semTake (pDrbFsFd->semId, WAIT_FOREVER) == ERROR)
    	return (ERROR);

    _WRS_TASK_ID_CURRENT_GET(pTcb);
    isRtpRequest = (pTcb != NULL) && !IS_KERNEL_TASK(pTcb);

    if (isRtpRequest)
	    {
	    if (pDrbFsFd->altBuf == NULL) 
	        {
	        /* allocate the alternate buffer */

	        if ((pDrbFsFd->altBuf = malloc (drbFsAltBufSize)) == NULL)
		        {	
		        (void) semGive (pDrbFsFd->semId);
		        (void) errnoSet (ENOMEM);
		        return 0; 
		        }
	        memset (pDrbFsFd->altBuf, 0, drbFsAltBufSize);
	        }
	    oneShotMaxSize = drbFsAltBufSize;
	    }
    else
	    {
	    /* 
	     * to read requests size is limited to avoid blocking the value-add
	     * for a long time.
	     */

	    oneShotMaxSize = drbFsMaxRWBufSize;
	    }

    /* on first read, get file from server */
    
    if (pDrbFsFd->offset == 0)
        {
    	drbClient * cli;
    	void *      output;
    	int         err;
        if ((cli = drbFsClientGet (pDrbFsFd)) == NULL)
        	{
	        (void) semGive (pDrbFsFd->semId);
	        (void) errnoSet (ENOMEM);
	        return 0;             
        	}
        memStreamInit (&pDrbFsFd->stream);
        
        output = NULL;
        err = drbGetFile(cli, &output,
                         DRBOPT_PATH, pDrbFsFd->fd,
                         DRBOPT_IO_DATA, &pDrbFsFd->stream,
                         DRBOPT_IO_FUNC, memStreamWrite,
                         DRBOPT_END);

		if (err != DRBERR_OK) 
			{
			DRBFS_DEBUG_LOG ("File download error (%d): %s\n", err, (char*)output);
			free(output);
	        (void) semGive (pDrbFsFd->semId);
	        (void) errnoSet (ENOMEM);
	        return 0;             
			} 
        }

    /* Call the Read API of the filesystem proxy. */

    while (bytesRead < maxBytes)
	    {
	    if (pDrbFsFd->offset > pDrbFsFd->stream.size)
	    	break;

	    oneShotReadBuf = (isRtpRequest) ? pDrbFsFd->altBuf : (buf + bytesRead);
	    oneShotReadCnt = -1;
	    oneShotReadSize = min ((maxBytes - bytesRead), oneShotMaxSize);
	    oneShotReadSize = min (pDrbFsFd->stream.size - pDrbFsFd->offset, oneShotReadSize);
	    
        bcopy (pDrbFsFd->stream.data + pDrbFsFd->offset, oneShotReadBuf, oneShotReadSize);
        oneShotReadCnt = oneShotReadSize;
        if (oneShotReadCnt == 0)
        	break;
        
	    if (isRtpRequest)
	        bcopy (oneShotReadBuf, buf + bytesRead, oneShotReadCnt);

	    /* update file offset */

	    pDrbFsFd->offset += oneShotReadCnt;
	    bytesRead += oneShotReadCnt;
	    }
 
    (void) semGive (pDrbFsFd->semId);

    DRBFS_DEBUG_LOG ("%d bytes read\n", bytesRead);
    if ((readStatus != 0) && (bytesRead == 0))
        return ((ssize_t)ERROR);
    return (bytesRead);
    }

static ssize_t drbFsWrite
    (
    DRBFS_FILE_DESC *	pDrbFsFd,	/* channel handle */
    char *		        buf,		/* buffer of data to be sent */
    size_t		        maxBytes	/* max bytes to transfer */
    )
    {
    size_t 	maxRequestSize;		    /* max size for a write request */
    size_t 	bytesWritten = 0;	    /* total written bytes */
    size_t 	oneShotWriteSize = 0;	/* bytes written by block */
    char *	oneShotWriteBuf;	    /* data to write buffer */
    BOOL 	isRtpRequest;
    WIND_TCB *	pTcb;

	DRBFS_DEBUG_LOG ("Dropbox FS Writing - name: %s, buf: %s, maxBytes: %i\n",
			         pDrbFsFd->pathname, buf, maxBytes);

    if (!isMounted (pDrbFsFd->pDev))
	    {
	    (void) errnoSet (EAGAIN); 
  	    return (ERROR);	
	    }

    if (semTake (pDrbFsFd->semId, WAIT_FOREVER) == ERROR)
    	return (ERROR);

    _WRS_TASK_ID_CURRENT_GET(pTcb);
    isRtpRequest = (pTcb != NULL) && !IS_KERNEL_TASK(pTcb);

    if (isRtpRequest)
    	{
     	/*
    	 * For RTP access, an alternate buffer is used because buffer provided
    	 * as parameter won't be accessible from TCF dispatch thread.
    	 */

    	if (pDrbFsFd->altBuf == NULL)
    	    {
	    /* allocate the alternate buffer */

    	    if ((pDrbFsFd->altBuf = malloc (drbFsAltBufSize)) == NULL)
                {
	   	        (void) semGive (pDrbFsFd->semId);
    		    (void) errnoSet (ENOMEM);
    		    return 0;
    		    }
    	    memset (pDrbFsFd->altBuf, 0, drbFsAltBufSize);
    	    }
    	maxRequestSize = drbFsAltBufSize;
    	}
    else
    	{
    	/*
    	 * to read requests size is limited to avoid blocking too much time
    	 * the value-add.
    	 */

    	maxRequestSize = drbFsMaxRWBufSize;
    	}

    /* if (pDrbFsFd->offset == 0)
    	memStreamInit(&pDrbFsFd->stream); */
    
    while (bytesWritten < maxBytes)
   	    {
	    oneShotWriteSize = min (maxRequestSize, (maxBytes - bytesWritten));
	    oneShotWriteBuf = (isRtpRequest) ? pDrbFsFd->altBuf : buf + bytesWritten;

	    if (isRtpRequest)
	        {
	        /* copy data to alternate buffer */

	        bcopy (buf + bytesWritten, pDrbFsFd->altBuf, oneShotWriteSize);
	        }

	    bytesWritten = memStreamWrite (oneShotWriteBuf, oneShotWriteSize, 1, &pDrbFsFd->stream);

    	/* update file offset */

	    pDrbFsFd->offset += bytesWritten;
       	}

    (void) semGive (pDrbFsFd->semId);

    DRBFS_DEBUG_LOG ("%d bytes bytes written\n", bytesWritten);

    if (bytesWritten == 0)
    	return ((ssize_t) ERROR);
    return ((ssize_t) bytesWritten);
    }

static STATUS drbFsDirRead
    (
    DRBFS_FILE_DESC *	pDrbFsFd,    /* file handle */
    DIR	*		        pDir         /* ptr to directory descriptor */
    )
    {
    int               status = 0;
    int               posixErrno = 0;
    void *            output;
    int               err;
    drbMetadata *     meta;
    drbClient *       cli;
    
    if ((pDir == NULL) || (pDrbFsFd->pathname == NULL))
	    {
	    DRBFS_DEBUG_LOG ("Read a virtual I/O channel directory. Parameter error\n");
        (void) errnoSet (EINVAL);
    	return (ERROR);
	    } 

    DRBFS_DEBUG_LOG ("Read a virtual I/O channel directory, name = %s\n",
		             pDrbFsFd->pathname);

    if (!isMounted (pDrbFsFd->pDev))
	    {
	    (void) errnoSet (EAGAIN); 
  	    return (ERROR);	
	    }

    if (semTake (pDrbFsFd->semId, WAIT_FOREVER) == ERROR)
    	return (ERROR);

	if (pDrbFsFd->fd == NULL)
		{
	    struct FileNode * lastNode = NULL;
	    struct FileNode * newNode = NULL;

	    if (IS_DEVICE_PATH (pDrbFsFd->pathname))
	        {
            /*
	         * special case:  target path is the drbFs device name 
	         *
	         * file list is the mount points list. If multiple auto 
	         * mounted mount point exists (named 'root' or 
	         * 'root/<LETTER>, a single 'root' file is added to file list
	         * to create a pseudo 'root' directory.
	         *
	         * A fake directory descriptor is created because
	         * it will not be managed by the FileSytem proxy.
	         */

	        unsigned int ix;
            BOOL         rootAdded = FALSE;

			DRBFS_DEBUG_LOG("Create a fake file list with mount points.\n");
	
			/* create a fake file name list with only the mount point name */
	
			for (ix = 0; ix < MAX_MNT_PER_DEV; ix++)
				{
				BOOL isAutoMounted = FALSE;
	
				if (IS_MNT_FREE (pDrbFsFd->pDev->mntTbl[ix]))
					continue;
			
				isAutoMounted = IS_AUTO_MNT (pDrbFsFd->pDev->mntTbl[ix]);
	
				if (rootAdded && isAutoMounted)
				   continue;
	
				/* 
				 * newNode is a new allocated file name node to insert into 
				 * pDrbFsFd->fileList linked list. So is it expected to 
				 * overwrite the pointer each time a new node is created 
				 */
	 
				newNode = malloc (sizeof (FileNode));
				memset (newNode, 0, sizeof (FileNode));
				if (isAutoMounted)
					{
					newNode->file_name = strdup (ROOT_DIR);
					rootAdded = TRUE;
					}
				else
					newNode->file_name = strdup (pDrbFsFd->pDev->mntTbl[ix].name);
	
				if (pDrbFsFd->fileList == NULL)
					{
					/* file list not yet created: initialize it */
	
					pDrbFsFd->fileList = newNode;
					pDrbFsFd->curNode = pDrbFsFd->fileList;
					}
	
				if (lastNode == NULL)
					{
					/* last node pointer not yet initialized */
	
					lastNode = pDrbFsFd->fileList;
					}
				else
					{
					/* last node pointer to point on lastet element */
	
					lastNode->next = newNode;
					lastNode = newNode;
					}
				}
			    pDrbFsFd->fd = strdup (FAKE_DIR_FD);
			}
		else
			{
			char * 	hostPath = NULL;
			BOOL 	isRdMnt;
	
			if (IS_ROOT_PATH (pDrbFsFd->pathname))
				{
				unsigned int ix;
	
				/* 
				 * special case:  target path is auto-mounted host drive path:
				 * the <hostfs device>/root.
				 *
				 * If value-add is running on Windows, one or several
				 * 'root/<LETTER>' mount point exists, file list is
				 * the <LETTER> list. A fake directory descriptor is created
				 * because it will not be managed by the FileSytem proxy.
				 *
				 */
	
				for (ix = 0; ix < MAX_MNT_PER_DEV; ix++)
					{
					if (IS_MNT_FREE (pDrbFsFd->pDev->mntTbl[ix]))
						continue;
	
					if (IS_WIN_AUTO_MNT (pDrbFsFd->pDev->mntTbl[ix]))
						{
						char * mntName = pDrbFsFd->pDev->mntTbl[ix].name;
						char * dLetter = &mntName[strlen (ROOT_DIR) + 1];
	
						/* 
						 * newNode is a new allocated file name node to insert 
						 * into pDrbFsFd->fileList linked list. So is it 
						 * expected to overwrite the pointer each time a new 
						 * node is created.
						 */
	 
						/* coverity[overwrite_var] */
						newNode = malloc (sizeof (FileNode));
						memset (newNode, 0, sizeof (FileNode));
						newNode->file_name = strdup (dLetter);
	
						if (pDrbFsFd->fileList == NULL)
							{
							pDrbFsFd->fileList = newNode;
							pDrbFsFd->curNode = pDrbFsFd->fileList;
							}
	
						if (lastNode == NULL)
							{
							/* last node pointer not yet initialized */
	
							lastNode = pDrbFsFd->fileList;
							}
						else
							{
							/* last node pointer to point on latest element */
	
							lastNode->next = newNode;
							lastNode = newNode;
							}
						if (pDrbFsFd->fd == NULL)
							pDrbFsFd->fd = strdup (FAKE_DIR_FD);
						}
					}
				}
	
			if (pDrbFsFd->fd == NULL)
			    {
				/* Regular case: Query the value-add to create the file list */
	
				if ((hostPath = toHostPath (pDrbFsFd->pDev, pDrbFsFd->pathname,
											&isRdMnt)) == NULL)
					{
					(void) semGive (pDrbFsFd->semId);
					return (ERROR); 
					}
		
		        if ((cli = drbFsClientGet (pDrbFsFd)) == NULL)
		        	{
		        	(void) semGive (pDrbFsFd->semId);
		            return (ERROR);
		        	}

				output = NULL;
				err = drbGetMetadata (cli, &output,
									  DRBOPT_PATH, hostPath,
									  DRBOPT_LIST, true,
									  //                     DRBOPT_FILE_LIMIT, 100,
									  DRBOPT_END);
				if (err != DRBERR_OK) 
					{
					DRBFS_DEBUG_LOG ("Metadata error (%d): %s\n", err, (char*)output);
					free(output);
					} 
				else 
					{
					meta = (drbMetadata*)output;
					// displayMetadata(meta, "Metadata");
					// drbDestroyMetadata(meta, true);
					}
		
			    /* create the files name list for this directory. */
		
				if ((status == 0) && (pDrbFsFd->fileList == NULL) && (meta->contents))
					{
					for (int i = 0; i < meta->contents->size; i++) 
						{
						newNode = malloc (sizeof (FileNode));
						memset (newNode, 0, sizeof (FileNode));
						newNode->file_name = strdup (&meta->contents->array[i]->path[strlen(hostPath) + 1]);
						if (pDrbFsFd->fileList == NULL)
							{
							pDrbFsFd->fileList = newNode;
							pDrbFsFd->curNode = pDrbFsFd->fileList;
							}
						if (lastNode == NULL)
							lastNode = pDrbFsFd->fileList;
						else
							{
							lastNode->next = newNode;
							lastNode = newNode;
							}
						}
					}
				drbDestroyMetadata(meta, true);
				pDrbFsFd->fd = strdup (hostPath); 
				free ((void *) hostPath);
			    }
			}
		}
	
    pDrbFsFd->isDirFd = TRUE;

    if ((status != 0) || (pDrbFsFd->curNode == NULL))
	    {
	    /* 
	     * If an error has been returned by a FileSystem proxy call,
   	     * convert it to POSIX errno.
	     */
	    if (status != 0)
	        {
 	        if (posixErrno > 0)
	            (void) errnoSet (posixErrno);
	
	        DRBFS_DEBUG_LOG ("Error opening directory = %s\n (errno= 0x%x)\n",
		                     pDrbFsFd->pathname, posixErrno);
	        }
	    /* 
	     * specify that all files of the directory have already been 
	     * provided.
    	 * if "status != 0", this is somewhat ugly, but is about the best we 
	     * can do at the moment: we assume any failure is due to 
	     * end-of-directory.
	     */
	
	    pDir->dd_eof = TRUE;
	    (void) semGive (pDrbFsFd->semId);
	    return (OK);
	    }

    /*  provide next file name. */

    if (strncpy (pDir->dd_dirent.d_name, pDrbFsFd->curNode->file_name, 
	             strlen (pDrbFsFd->curNode->file_name) + 1) == NULL)
	    {
        (void) errnoSet (ENOMEM);
	    pDir->dd_eof = TRUE;
	    (void) semGive (pDrbFsFd->semId);
	    return (OK);
	    }

    pDir->dd_dirent.d_name[NAME_MAX] = EOS;

    /* update "current file " pointer to point to the next file */

    pDrbFsFd->curNode = pDrbFsFd->curNode->next;

    (void) semGive (pDrbFsFd->semId);
    return (OK);
    }

static STATUS drbFsIoctl
    (
    DRBFS_FILE_DESC *	pDrbFsFd,	/* device to control */
    int 		        request,	/* request code */
    _Vx_ioctl_arg_t	    arg		    /* some argument */
    )
    {
    int			status = 0;

	DRBFS_DEBUG_LOG ("Dropbox FS IOCTL - name: %s, request: %i, arg: %i\n",
			         pDrbFsFd->pathname, request, arg);

	if (!isMounted (pDrbFsFd->pDev))
	    {
	    (void) errnoSet (EAGAIN); 
  	    return (ERROR);	
	    }

    if (semTake (pDrbFsFd->semId, WAIT_FOREVER) == ERROR)
    	return (ERROR);

    switch (request)
	    {
	    case FIOSEEK:           /* set file read pointer */
	        DRBFS_DEBUG_LOG ("File offset updated from : %d, to : %d\n", 
		                     (int) pDrbFsFd->offset, arg);

	        pDrbFsFd->offset = (off_t) arg;
	        break;
	    case FIOWHERE:          /* return current seek pointer */
	        DRBFS_DEBUG_LOG ("Current File offset : %d\n", (int) pDrbFsFd->offset);
	        (void) semGive (pDrbFsFd->semId);
	        return ((STATUS) pDrbFsFd->offset);
	    case FIOGETFL:
	        DRBFS_DEBUG_LOG ("File open mode : 0x%x\n", pDrbFsFd->openmode);
	        /* this one is handled locally. */

	        *((int *) arg) = pDrbFsFd->openmode;
	        break;
	    case FIOFSTATGET :
	        {
	        struct stat * pStat = (struct stat *) arg;
	        memset (pStat, 0, sizeof (struct stat));

	        status = drbFsStat (pDrbFsFd, pStat);
	        break;
	        }
	    case FIOUNLINK:
    	    status = drbFsDelete (pDrbFsFd->pDev, (char *)pDrbFsFd->pathname);
	        break;
        case FIOREADDIR:
	        status = drbFsDirRead (pDrbFsFd, (DIR *) arg);
	        break;
	    default:                /* return error for all others */
	        {
	        (void) errnoSet (ENOTSUP);
	        status = ERROR;
	        }
	    } 

    DRBFS_DEBUG_LOG ("Ioctl status %d\n", status);
    (void) semGive (pDrbFsFd->semId);
    return status;
    }
