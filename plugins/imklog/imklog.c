/* The kernel log module.
 *
 * This is an abstracted module. As Linux and BSD kernel log is conceptually the
 * same, we do not do different input plugins for them but use
 * imklog in both cases, just with different "backend drivers" for
 * the different platforms. This also enables a rsyslog.conf to
 * be used on multiple platforms without the need to take care of
 * what the kernel log is coming from.
 *
 * See platform-specific files (e.g. linux.c, bsd.c) in the plugin's
 * working directory. For other systems with similar kernel logging
 * functionality, no new input plugin shall be written but rather a
 * driver be developed for imklog. Please note that imklog itself is
 * mostly concerned with handling the interface. Any real action happens
 * in the drivers, as things may be pretty different on different
 * platforms.
 *
 * Please note that this file replaces the klogd daemon that was
 * also present in pre-v3 versions of rsyslog.
 *
 * To test under Linux:
 * echo test1 > /dev/kmsg
 *
 * Copyright (C) 2008-2012 Adiscon GmbH
 *
 * This file is part of rsyslog.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *       http://www.apache.org/licenses/LICENSE-2.0
 *       -or-
 *       see COPYING.ASL20 in the source distribution
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "config.h"
#include "rsyslog.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <stdlib.h>
#include <sys/socket.h>

#include "dirty.h"
#include "cfsysline.h"
#include "obj.h"
#include "msg.h"
#include "module-template.h"
#include "datetime.h"
#include "imklog.h"
#include "net.h"
#include "glbl.h"
#include "prop.h"
#include "errmsg.h"
#include "unicode-helper.h"

MODULE_TYPE_INPUT
MODULE_TYPE_NOKEEP
MODULE_CNFNAME("imklog")

/* Module static data */
DEF_IMOD_STATIC_DATA
DEFobjCurrIf(datetime)
DEFobjCurrIf(glbl)
DEFobjCurrIf(prop)
DEFobjCurrIf(net)
DEFobjCurrIf(errmsg)

/* config settings */
typedef struct configSettings_s {
	int bPermitNonKernel; /* permit logging of messages not having LOG_KERN facility */
	int bParseKernelStamp; /* if try to parse kernel timestamps for message time */
	int bKeepKernelStamp; /* keep the kernel timestamp in the message */
	int iFacilIntMsg; /* the facility to use for internal messages (set by driver) */
	uchar *pszPath;
	int console_log_level; /* still used for BSD */
} configSettings_t;
static configSettings_t cs;

static modConfData_t *loadModConf = NULL;/* modConf ptr to use for the current load process */
static modConfData_t *runModConf = NULL;/* modConf ptr to use for the current load process */
static int bLegacyCnfModGlobalsPermitted;/* are legacy module-global config parameters permitted? */

/* module-global parameters */
static struct cnfparamdescr modpdescr[] = {
	{ "logpath", eCmdHdlrGetWord, 0 },
	{ "permitnonkernelfacility", eCmdHdlrBinary, 0 },
	{ "consoleloglevel", eCmdHdlrInt, 0 },
	{ "internalmsgfacility", eCmdHdlrFacility, 0 }
};
static struct cnfparamblk modpblk =
	{ CNFPARAMBLK_VERSION,
	  sizeof(modpdescr)/sizeof(struct cnfparamdescr),
	  modpdescr
	};

static prop_t *pInputName = NULL;	/* there is only one global inputName for all messages generated by this module */
static prop_t *pLocalHostIP = NULL;	/* a pseudo-constant propterty for 127.0.0.1 */

static inline void
initConfigSettings(void)
{
	cs.bPermitNonKernel = 0;
	cs.bParseKernelStamp = 0;
	cs.bKeepKernelStamp = 0;
	cs.console_log_level = -1;
	cs.pszPath = NULL;
	cs.iFacilIntMsg = klogFacilIntMsg();
}


/* enqueue the the kernel message into the message queue.
 * The provided msg string is not freed - thus must be done
 * by the caller.
 * rgerhards, 2008-04-12
 */
static rsRetVal
enqMsg(uchar *msg, uchar* pszTag, int iFacility, int iSeverity, struct timeval *tp)
{
	struct syslogTime st;
	msg_t *pMsg;
	DEFiRet;

	assert(msg != NULL);
	assert(pszTag != NULL);

	if(tp == NULL) {
		CHKiRet(msgConstruct(&pMsg));
	} else {
		datetime.timeval2syslogTime(tp, &st);
		CHKiRet(msgConstructWithTime(&pMsg, &st, tp->tv_sec));
	}
	MsgSetFlowControlType(pMsg, eFLOWCTL_LIGHT_DELAY);
	MsgSetInputName(pMsg, pInputName);
	MsgSetRawMsgWOSize(pMsg, (char*)msg);
	MsgSetMSGoffs(pMsg, 0);	/* we do not have a header... */
	MsgSetRcvFrom(pMsg, glbl.GetLocalHostNameProp());
	MsgSetRcvFromIP(pMsg, pLocalHostIP);
	MsgSetHOSTNAME(pMsg, glbl.GetLocalHostName(), ustrlen(glbl.GetLocalHostName()));
	MsgSetTAG(pMsg, pszTag, ustrlen(pszTag));
	pMsg->iFacility = iFacility;
	pMsg->iSeverity = iSeverity;
	CHKiRet(submitMsg(pMsg));

finalize_it:
	RETiRet;
}

/* parse the PRI from a kernel message. At least BSD seems to have
 * non-kernel messages inside the kernel log...
 * Expected format: "<pri>". piPri is only valid if the function 
 * successfully returns. If there was a proper pri ppSz is advanced to the
 * position right after ">".
 * rgerhards, 2008-04-14
 */
static rsRetVal
parsePRI(uchar **ppSz, int *piPri)
{
	DEFiRet;
	int i;
	uchar *pSz;

	assert(ppSz != NULL);
	pSz = *ppSz;
	assert(pSz != NULL);
	assert(piPri != NULL);

	if(*pSz != '<' || !isdigit(*(pSz+1)))
		ABORT_FINALIZE(RS_RET_INVALID_PRI);

	++pSz;
	i = 0;
	while(isdigit(*pSz)) {
		i = i * 10 + *pSz++ - '0';
	}

	if(*pSz != '>')
		ABORT_FINALIZE(RS_RET_INVALID_PRI);

	/* OK, we have a valid PRI */
	*piPri = i;
	*ppSz = pSz + 1; /* update msg ptr to position after PRI */

finalize_it:
	RETiRet;
}


/* log an imklog-internal message
 * rgerhards, 2008-04-14
 */
rsRetVal imklogLogIntMsg(int priority, char *fmt, ...)
{
	DEFiRet;
	va_list ap;
	uchar msgBuf[2048]; /* we use the same size as sysklogd to remain compatible */

	va_start(ap, fmt);
	vsnprintf((char*)msgBuf, sizeof(msgBuf) / sizeof(char), fmt, ap);
	va_end(ap);

	logmsgInternal(NO_ERRCODE ,priority, msgBuf, 0);

	RETiRet;
}


/* log a kernel message. If tp is non-NULL, it contains the message creation
 * time to use.
 * rgerhards, 2008-04-14
 */
rsRetVal Syslog(int priority, uchar *pMsg, struct timeval *tp)
{
	int pri = -1;
	rsRetVal localRet;
	DEFiRet;

	/* then check if we have two PRIs. This can happen in case of systemd,
	 * in which case the second PRI is the right one.
	 */
	if(pMsg[3] == '<' || (pMsg[3] == ' ' && pMsg[4] == '<')) { /* could be a pri... */
		uchar *pMsgTmp = pMsg + ((pMsg[3] == '<') ? 3 : 4);
		localRet = parsePRI(&pMsgTmp, &pri);
		if(localRet == RS_RET_OK && pri >= 8 && pri <= 192) {
			/* *this* is our PRI */
			DBGPRINTF("imklog detected secondary PRI(%d) in klog msg\n", pri);
			pMsg = pMsgTmp;
			priority = pri;
		}
	}
	if(pri == -1) {
		localRet = parsePRI(&pMsg, &priority);
		if(localRet != RS_RET_INVALID_PRI && localRet != RS_RET_OK)
			FINALIZE;
	}
	/* if we don't get the pri, we use whatever we were supplied */

	/* ignore non-kernel messages if not permitted */
	if(cs.bPermitNonKernel == 0 && LOG_FAC(priority) != LOG_KERN)
		FINALIZE; /* silently ignore */

	iRet = enqMsg((uchar*)pMsg, (uchar*) "kernel:", LOG_FAC(priority), LOG_PRI(priority), tp);

finalize_it:
	RETiRet;
}


/* helper for some klog drivers which need to know the MaxLine global setting. They can
 * not obtain it themselfs, because they are no modules and can not query the object hander.
 * It would probably be a good idea to extend the interface to support it, but so far
 * we create a (sufficiently valid) work-around. -- rgerhards, 2008-11-24
 */
int klog_getMaxLine(void)
{
	return glbl.GetMaxLine();
}


BEGINrunInput
CODESTARTrunInput
	/* this is an endless loop - it is terminated when the thread is
	 * signalled to do so. This, however, is handled by the framework,
	 * right into the sleep below.
	 */
	while(!pThrd->bShallStop) {
		/* klogLogKMsg() waits for the next kernel message, obtains it
                 * and then submits it to the rsyslog main queue.
	   	 * rgerhards, 2008-04-09
	   	 */
                CHKiRet(klogLogKMsg(runModConf));
	}
finalize_it:
ENDrunInput


BEGINbeginCnfLoad
CODESTARTbeginCnfLoad
	loadModConf = pModConf;
	pModConf->pConf = pConf;
	/* init our settings */
	pModConf->pszPath = NULL;
	pModConf->bPermitNonKernel = 0;
	pModConf->bParseKernelStamp = 0;
	pModConf->bKeepKernelStamp = 0;
	pModConf->console_log_level = -1;
	pModConf->iFacilIntMsg = klogFacilIntMsg();
	loadModConf->configSetViaV2Method = 0;
	bLegacyCnfModGlobalsPermitted = 1;
	/* init legacy config vars */
	initConfigSettings();
ENDbeginCnfLoad


BEGINsetModCnf
	struct cnfparamvals *pvals = NULL;
	int i;
CODESTARTsetModCnf
	pvals = nvlstGetParams(lst, &modpblk, NULL);
	if(pvals == NULL) {
		errmsg.LogError(0, RS_RET_MISSING_CNFPARAMS, "error processing module "
				"config parameters [module(...)]");
		ABORT_FINALIZE(RS_RET_MISSING_CNFPARAMS);
	}

	if(Debug) {
		dbgprintf("module (global) param blk for imklog:\n");
		cnfparamsPrint(&modpblk, pvals);
	}

	for(i = 0 ; i < modpblk.nParams ; ++i) {
		if(!pvals[i].bUsed)
			continue;
		if(!strcmp(modpblk.descr[i].name, "logpath")) {
			loadModConf->pszPath = (uchar*)es_str2cstr(pvals[i].val.d.estr, NULL);
		} else if(!strcmp(modpblk.descr[i].name, "permitnonkernelfacility")) {
			loadModConf->bPermitNonKernel = (int) pvals[i].val.d.n;
		} else if(!strcmp(modpblk.descr[i].name, "consoleloglevel")) {
			loadModConf->console_log_level= (int) pvals[i].val.d.n;
		} else if(!strcmp(modpblk.descr[i].name, "internalmsgfacility")) {
			loadModConf->iFacilIntMsg = (int) pvals[i].val.d.n;
		} else {
			dbgprintf("imklog: program error, non-handled "
			  "param '%s' in beginCnfLoad\n", modpblk.descr[i].name);
		}
	}

	/* disable legacy module-global config directives */
	bLegacyCnfModGlobalsPermitted = 0;
	loadModConf->configSetViaV2Method = 1;

finalize_it:
	if(pvals != NULL)
		cnfparamvalsDestruct(pvals, &modpblk);
ENDsetModCnf


BEGINendCnfLoad
CODESTARTendCnfLoad
	if(!loadModConf->configSetViaV2Method) {
		/* persist module-specific settings from legacy config system */
		loadModConf->bPermitNonKernel = cs.bPermitNonKernel;
		loadModConf->bParseKernelStamp = cs.bParseKernelStamp;
		loadModConf->bKeepKernelStamp = cs.bKeepKernelStamp;
		loadModConf->iFacilIntMsg = cs.iFacilIntMsg;
		loadModConf->console_log_level = cs.console_log_level;
		if((cs.pszPath == NULL) || (cs.pszPath[0] == '\0')) {
			loadModConf->pszPath = NULL;
			if(cs.pszPath != NULL)
				free(cs.pszPath);
		} else {
			loadModConf->pszPath = cs.pszPath;
		}
		cs.pszPath = NULL;
	}

	loadModConf = NULL; /* done loading */
ENDendCnfLoad


BEGINcheckCnf
CODESTARTcheckCnf
ENDcheckCnf


BEGINactivateCnfPrePrivDrop
CODESTARTactivateCnfPrePrivDrop
	runModConf = pModConf;
        iRet = klogWillRun(runModConf);
ENDactivateCnfPrePrivDrop


BEGINactivateCnf
CODESTARTactivateCnf
ENDactivateCnf


BEGINfreeCnf
CODESTARTfreeCnf
ENDfreeCnf


BEGINwillRun
CODESTARTwillRun
ENDwillRun


BEGINafterRun
CODESTARTafterRun
        iRet = klogAfterRun(runModConf);
ENDafterRun


BEGINmodExit
CODESTARTmodExit
	if(pInputName != NULL)
		prop.Destruct(&pInputName);
	if(pLocalHostIP != NULL)
		prop.Destruct(&pLocalHostIP);

	/* release objects we used */
	objRelease(glbl, CORE_COMPONENT);
	objRelease(net, CORE_COMPONENT);
	objRelease(datetime, CORE_COMPONENT);
	objRelease(prop, CORE_COMPONENT);
	objRelease(errmsg, CORE_COMPONENT);
ENDmodExit


BEGINqueryEtryPt
CODESTARTqueryEtryPt
CODEqueryEtryPt_STD_IMOD_QUERIES
CODEqueryEtryPt_STD_CONF2_QUERIES
CODEqueryEtryPt_STD_CONF2_setModCnf_QUERIES
CODEqueryEtryPt_STD_CONF2_PREPRIVDROP_QUERIES
ENDqueryEtryPt

static rsRetVal resetConfigVariables(uchar __attribute__((unused)) *pp, void __attribute__((unused)) *pVal)
{
	cs.bPermitNonKernel = 0;
	cs.bParseKernelStamp = 0;
	cs.bKeepKernelStamp = 0;
	if(cs.pszPath != NULL) {
		free(cs.pszPath);
		cs.pszPath = NULL;
	}
	cs.iFacilIntMsg = klogFacilIntMsg();
	return RS_RET_OK;
}

BEGINmodInit()
CODESTARTmodInit
	*ipIFVersProvided = CURR_MOD_IF_VERSION; /* we only support the current interface specification */
CODEmodInit_QueryRegCFSLineHdlr
	CHKiRet(objUse(datetime, CORE_COMPONENT));
	CHKiRet(objUse(glbl, CORE_COMPONENT));
	CHKiRet(objUse(prop, CORE_COMPONENT));
	CHKiRet(objUse(net, CORE_COMPONENT));
	CHKiRet(objUse(errmsg, CORE_COMPONENT));

	/* we need to create the inputName property (only once during our lifetime) */
	CHKiRet(prop.CreateStringProp(&pInputName, UCHAR_CONSTANT("imklog"), sizeof("imklog") - 1));
	CHKiRet(prop.CreateStringProp(&pLocalHostIP, UCHAR_CONSTANT("127.0.0.1"), sizeof("127.0.0.1") - 1));

	/* init legacy config settings */
	initConfigSettings();

	CHKiRet(omsdRegCFSLineHdlr((uchar *)"debugprintkernelsymbols", 0, eCmdHdlrGoneAway,
			NULL, NULL, STD_LOADABLE_MODULE_ID));
	CHKiRet(regCfSysLineHdlr2((uchar *)"klogpath", 0, eCmdHdlrGetWord,
			NULL, &cs.pszPath, STD_LOADABLE_MODULE_ID, &bLegacyCnfModGlobalsPermitted));
	CHKiRet(omsdRegCFSLineHdlr((uchar *)"klogsymbollookup", 0, eCmdHdlrGoneAway,
			NULL, NULL, STD_LOADABLE_MODULE_ID));
	CHKiRet(omsdRegCFSLineHdlr((uchar *)"klogsymbolstwice", 0, eCmdHdlrGoneAway,
			NULL, NULL, STD_LOADABLE_MODULE_ID));
	CHKiRet(omsdRegCFSLineHdlr((uchar *)"klogusesyscallinterface", 0, eCmdHdlrGoneAway,
			NULL, NULL, STD_LOADABLE_MODULE_ID));
	CHKiRet(regCfSysLineHdlr2((uchar *)"klogpermitnonkernelfacility", 0, eCmdHdlrBinary,
			NULL, &cs.bPermitNonKernel, STD_LOADABLE_MODULE_ID, &bLegacyCnfModGlobalsPermitted));
	CHKiRet(regCfSysLineHdlr2((uchar *)"klogconsoleloglevel", 0, eCmdHdlrInt,
			NULL, &cs.console_log_level, STD_LOADABLE_MODULE_ID, &bLegacyCnfModGlobalsPermitted));
	CHKiRet(regCfSysLineHdlr2((uchar *)"kloginternalmsgfacility", 0, eCmdHdlrFacility,
			NULL, &cs.iFacilIntMsg, STD_LOADABLE_MODULE_ID, &bLegacyCnfModGlobalsPermitted));
	CHKiRet(regCfSysLineHdlr2((uchar *)"klogparsekerneltimestamp", 0, eCmdHdlrBinary,
			NULL, &cs.bParseKernelStamp, STD_LOADABLE_MODULE_ID, &bLegacyCnfModGlobalsPermitted));
	CHKiRet(regCfSysLineHdlr2((uchar *)"klogkeepkerneltimestamp", 0, eCmdHdlrBinary,
			NULL, &cs.bKeepKernelStamp, STD_LOADABLE_MODULE_ID, &bLegacyCnfModGlobalsPermitted));
	CHKiRet(omsdRegCFSLineHdlr((uchar *)"resetconfigvariables", 1, eCmdHdlrCustomHandler,
			resetConfigVariables, NULL, STD_LOADABLE_MODULE_ID));
ENDmodInit
/* vim:set ai:
 */
