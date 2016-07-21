# vxDropBox

## Introduction

This repository contains a library that interfaces VxWorks with Dropbox.
With this library, your Dropbox is seen as a remote filesystem on the VxWorks target.

This repository gathers several packages needed to enable Dropbox on VxWorks:

* [C dropbox API](https://github.com/Dwii/Dropbox-C.git)
* [Jansson (JSON parsing)](https://github.com/akheron/jansson.git)
* [liboauth](https://sourceforge.net/projects/liboauth/files/liboauth-1.0.3.tar.gz)
* [asprintf](https://github.com/littlstar/asprintf.c.git).

## VxWorks configuration

1. Create and build a source build project, in which you add cURL library (`WEBCLI_CURL`), you need to add it with dependencies.
1. Create a kernel image project in which you add the following:
    * cURL library support (`INCLUDE_WEBCLI_CURL`)
    * DNS client (`INCLUDE_IPDNSC`) and set at least one valid DNS server IP address (`DNS_PRIMARY_NAME_SERVER`).
1. Create a DKM and import attached zip file. Set build spec to GNU (Diab toolchain was not tested). Add "-std=c99" to build options in Build Properties in order to be able to build recent open source code.
1. Include DKM project in kernel image project. This can be done by dragging and dropping in Project Explorer.
1. Build image.
1. Boot VxWorks.
 
## Dropbox configuration

1. Create a dropbox account on http://www.dropbox.com.
1. Go to https://www.dropbox.com/developers/apps and create an app.
1. In the settings, you will see an App key and App secret.
1. In VxWorks kernel shell, use `drbCreate()` to create a dropbox connection and pass app key and app secret strings as first and second parameters respectively:
```c
    -> drbCreate "xxxxxxxxxxxxxxxx", "yyyyyyyyyyyy"
```
1. You will be invited to connect to a temporary URL to grant access. Open this URL in a web browser and grant access.
1. A key and secret strings will be displayed in the console. These strings should be used as third and fourth parameters of `drbCreate()`. This will avoid to be asked to connect to temporary link each time target is rebooted.
1. You may want to automatically connect VxWorks target at boot time to Dropbox by calling `drbCreate()` in `usrAppInit()`. In such case, please increase tRootTask default stack size (`ROOT_STACK_SIZE`) or launch a new task. A stack size of 64KB (0x10000) always worked for me.
```c
    taskSpawn ("tDropbox", 100, 0, 0x10000, drbCreate,
               "xxxxxxxxxxxxxx", "yyyyyyyyyyyyy", "zzzzzzzzzzzzzzzz", "aaaaaaaaaaaaa",
               0, 0, 0, 0, 0, 0);
 ```
 
## Dropbox access

From kernel shell, you can now access to your dropbox:
```c
-> ls "/dropbox/root"
```

## Known Issues

This library is in BETA mode and still has some issues.
