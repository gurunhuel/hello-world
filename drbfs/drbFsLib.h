/*
 * drbFsLib.h
 *
 *  Created on: Apr 5, 2016
 *      Author: gurunhuel
 */

#ifndef DRBFSLIB_H_
#define DRBFSLIB_H_

extern STATUS drbFsLibInit(void);
extern STATUS drbFsDevInit (char *, char *, char **);
extern STATUS drbFsMountPointAdd (char *, char *, char *, BOOL, void *, char **);

#endif /* DRBFSLIB_H_ */
