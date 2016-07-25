/*
 * drbLib.c
 *
 *  Created on: Apr 5, 2016
 *      Author: gurunhuel
 */

#include <vxWorks.h>
#include <dropbox.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdio.h>
#include "drbFsLib.h"

#ifdef  DRB_DEBUG
#define DRB_DEBUG_ACCT_INFO(info)       displayAccountInfo (info)
#define DRB_DEBUG_META_LIST(list,title) displayMetadataList (list, title)
#define DRB_DEBUG_META(meta,title)      displayMetadata (meta, title)

static void displayMetadata(drbMetadata* meta, char* title);
#else
#define DRB_DEBUG_ACCT_INFO(info)
#define DRB_DEBUG_META_LIST(list,title)
#define DRB_DEBUG_META(meta,title)
#endif /* DRB_DEBUG */

#define DRB_DEV "/dropbox"
#define DRB_MNT "root"

#ifdef  DRB_DEBUG
static void displayAccountInfo(drbAccountInfo* info) 
    {
    if (info) 
        {
        printf("---------[ Account info ]---------\n");
        if(info->referralLink)         printf("referralLink: %s\n", info->referralLink);
        if(info->displayName)          printf("displayName:  %s\n", info->displayName);
        if(info->uid)                  printf("uid:          %d\n", *info->uid);
        if(info->country)              printf("country:      %s\n", info->country);
        if(info->email)                printf("email:        %s\n", info->email);
        if(info->quotaInfo.datastores) printf("datastores:   %u\n", *info->quotaInfo.datastores);
        if(info->quotaInfo.shared)     printf("shared:       %u\n", *info->quotaInfo.shared);
        if(info->quotaInfo.quota)      printf("quota:        %u\n", *info->quotaInfo.quota);
        if(info->quotaInfo.normal)     printf("normal:       %u\n", *info->quotaInfo.normal);
        }
    }

static char* strFromBool(bool b) { return b ? "true" : "false"; }

static void displayMetadataList(drbMetadataList* list, char* title) 
    {
    if (list)
        {
        printf("---------[ %s ]---------\n", title);
        for (int i = 0; i < list->size; i++) 
            displayMetadata(list->array[i], list->array[i]->path);
        }
    }

static void displayMetadata(drbMetadata* meta, char* title) 
    {
    if (meta) 
        {
        if(title) printf("---------[ %s ]---------\n", title);
        if(meta->hash)        printf("hash:        %s\n", meta->hash);
        if(meta->rev)         printf("rev:         %s\n", meta->rev);
        if(meta->thumbExists) printf("thumbExists: %s\n", strFromBool(*meta->thumbExists));
        if(meta->bytes)       printf("bytes:       %d\n", *meta->bytes);
        if(meta->modified)    printf("modified:    %s\n", meta->modified);
        if(meta->path)        printf("path:        %s\n", meta->path);
        if(meta->isDir)       printf("isDir:       %s\n", strFromBool(*meta->isDir));
        if(meta->icon)        printf("icon:        %s\n", meta->icon);
        if(meta->root)        printf("root:        %s\n", meta->root);
        if(meta->size)        printf("size:        %s\n", meta->size);
        if(meta->clientMtime) printf("clientMtime: %s\n", meta->clientMtime);
        if(meta->isDeleted)   printf("isDeleted:   %s\n", strFromBool(*meta->isDeleted));
        if(meta->mimeType)    printf("mimeType:    %s\n", meta->mimeType);
        if(meta->revision)    printf("revision:    %d\n", *meta->revision);
        if(meta->contents)    displayMetadataList(meta->contents, "Contents");
        }
    }
#endif /* DRB_DEBUG */

static STATUS drbAccessToken 
    (
    drbClient **      client,
	drbAccountInfo ** info,
	char *            c_key,
	char *            c_secret,
	char *            t_key,
	char *            t_secret
	)
    {
    int   err;
    void* output;
    
    // Global initialization
    drbInit();
    
    // Create a Dropbox client
    drbClient * cli = drbCreateClient(c_key, c_secret, t_key, t_secret);
    
    // Request a AccessToken if undefined (NULL)
    if (!t_key || !t_secret) 
        {
        drbOAuthToken* reqTok = drbObtainRequestToken(cli);
        
        if (reqTok) 
            {
            char * url = drbBuildAuthorizeUrl(reqTok);
            
            printf("Please visit %s\nThen press Enter...\n", url);
            free(url);
            fgetc(stdin);
            
            drbOAuthToken* accTok = drbObtainAccessToken(cli);
            
            if (accTok) 
                {
                // This key and secret can replace the NULL value in t_key and
                // t_secret for the next time.
                printf("key:    %s\nsecret: %s\n", accTok->key, accTok->secret);
                } 
            else
                {
                fprintf(stderr, "Failed to obtain an AccessToken...\n");
                return ERROR;
                }
            } 
        else 
            {
            fprintf(stderr, "Failed to obtain a RequestToken...\n");
            return ERROR;
            }
        }
    
    // Set default arguments to not repeat them on each API call
    drbSetDefault(cli, DRBOPT_ROOT, DRBVAL_ROOT_AUTO, DRBOPT_END);
    
    // Read account Informations
    output = NULL;
    err = drbGetAccountInfo(cli, &output, DRBOPT_END);
    if (err != DRBERR_OK) 
        {
        printf("Account info error (%d): %s\n", err, (char*)output);
        free(output);
        } 
    else 
#ifndef DRB_DEBUG
        *info = (drbAccountInfo *) output;
#else
        {
    	*info = (drbAccountInfo *) output;
        displayAccountInfo(*info);
        //drbDestroyAccountInfo(info);
        }

    // List root directory files
    output = NULL;
    err = drbGetMetadata(cli, &output,
                         DRBOPT_PATH, "/",
                         DRBOPT_LIST, true,
                         //                     DRBOPT_FILE_LIMIT, 100,
                         DRBOPT_END);
    if (err != DRBERR_OK) 
        {
        printf("Metadata error (%d): %s\n", err, (char*)output);
        free(output);
        } 
    else 
        {
        drbMetadata* meta = (drbMetadata*)output;
        displayMetadata(meta, "Metadata");
        drbDestroyMetadata(meta, true);
        }
#endif /* DRB_DEBUG */
     
    *client = cli;
    return OK;
    }

/******************************************************************************************************
 * drbCreate - create connection to Dropbox
 * 
 * This call creates a local device called /dropbox that connects to a Dropbox app based on the keys
 * you specified.
 * Before calling this routine you first need to create an app on Dropbox by going on
 * https://www.dropbox.com/developers/apps and create an app.
 * In app settings, look at App key and App secret, and set c_key and c_secret to these values
 * respectively. t_key and t_secret should be set to NULL the fist time drbCreate() is called.
 * At first call, key and secret should be displayed on the console. Please set t_key and t_secret
 * with these values respectively.
 * 
 * RETURNS: OK or ERROR.
 * 
 */

STATUS drbCreate 
    (
    char * c_key,    /* App key provided by Dropbox */
	char * c_secret, /* App secret provided by Dropbox */
	char * t_key,    /* key displayed on console at first call */
	char * t_secret  /* secret displayed on console at first call */
	) 
    {
	char *           devId = NULL;
	char *           mntId = NULL;
	drbClient *      cli;
	drbAccountInfo * info;

    if (drbFsLibInit() != OK)
    	{
    	printf ("Error initializing Dropbox FS Library.\n");
    	return ERROR;
    	}
    if (drbFsDevInit(DRB_DEV, NULL, &devId) != OK || devId == NULL)
        {
    	printf ("Error initializing Dropbox FS.\n");
    	return ERROR;
        }
    if (drbAccessToken (&cli, &info, c_key, c_secret, t_key, t_secret) != OK)
        {
    	printf ("Error getting Dropbox Token\n");
    	return ERROR;
        }
    if (drbFsMountPointAdd (devId, DRB_MNT, "", FALSE, (void *) cli, &mntId) != OK || mntId == NULL)
        {
    	printf ("Error mounting Dropbox FS\n");
    	return ERROR;
        }
    printf ("Dropbox: %s (owned by %s) successfully mounted on %s/%s\n", info->referralLink, info->displayName, DRB_DEV, DRB_MNT);
    return OK;
    }
