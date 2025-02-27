#if 0
INDI Driver Functions

Copyright (C) 2020 by Pawel Soja <kernel32.pl@gmail.com>
Copyright (C) 2003 - 2020 Jasem Mutlaq
Copyright (C) 2003 - 2006 Elwood C. Downey

This library is free software;
you can redistribute it and / or
modify it under the terms of the GNU Lesser General Public
License as published by the Free Software Foundation;
either
version 2.1 of the License, or (at your option) any later version.

This library is distributed in the hope that it will be useful,
     but WITHOUT ANY WARRANTY;
without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public
License along with this library;
if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110 - 1301  USA

#endif
#include "indidriver.h"

#include "base64.h"
#include "indicom.h"
#include "indidevapi.h"
#include "locale_compat.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <assert.h>

#include "userio.h"
#include "indiuserio.h"
#include "indidriverio.h"

int verbose;      /* chatty */
char *me = "";  /* a.out name */

#define MAXRBUF 2048

/*! INDI property type */
enum
{
INDI_NUMBER,
INDI_SWITCH,
INDI_TEXT,
INDI_LIGHT,
INDI_BLOB,
INDI_UNKNOWN
};

extern void waitPingReply(const char *);

// TODO use fast map
/* insure RO properties are never modified. RO Sanity Check */
typedef struct {
    char propName[MAXINDINAME];
    char devName[MAXINDIDEVICE];
    IPerm perm;
    const void *ptr;
    int type;
} ROSC;

static pthread_mutex_t rosc_mutex = PTHREAD_MUTEX_INITIALIZER;

static ROSC *propCache = NULL;
static int nPropCache = 0; /* # of elements in roCheck */

static ROSC *rosc_new()
{
    assert_mem(propCache = (ROSC *)(realloc(propCache, (nPropCache + 1) * sizeof *propCache)));
    return &propCache[nPropCache++];
}

static void rosc_add(const char *propName, const char *devName, IPerm perm, const void *ptr, int type)
{
    ROSC *SC = rosc_new();
    strcpy(SC->propName, propName);
    strcpy(SC->devName, devName);
    SC->perm = perm;
    SC->ptr  = ptr;
    SC->type = type;
}

/* Return pointer of property if already cached, NULL otherwise */
static ROSC *rosc_find(const char *propName, const char *devName)
{
    for (int i = 0; i < nPropCache; i++)
        if (!strcmp(propName, propCache[i].propName) && !strcmp(devName, propCache[i].devName))
            return &propCache[i];

    return NULL;
}

static void rosc_add_unique(const char *propName, const char *devName, IPerm perm, const void *ptr, int type)
{
    pthread_mutex_lock(&rosc_mutex);

    if (rosc_find(propName, devName) == NULL)
        rosc_add(propName, devName, perm, ptr, type);

    pthread_mutex_unlock(&rosc_mutex);
}

/* tell Client to delete the property with given name on given device, or
 * entire device if !name
 */
void IDDeleteVA(const char *dev, const char *name, const char *fmt, va_list ap)
{
    driverio io;
    driverio_init(&io);

    userio_xmlv1(&io.userio, io.user);
    IUUserIODeleteVA(&io.userio, io.user, dev, name, fmt, ap);

    driverio_finish(&io);
}

void IDDelete(const char *dev, const char *name, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    IDDeleteVA(dev, name, fmt, ap);
    va_end(ap);
}

/* tell indiserver we want to snoop on the given device/property.
 * name ignored if NULL or empty.
 */
void IDSnoopDevice(const char *snooped_device, const char *snooped_property)
{
    // Ignore empty snooped device
    if (snooped_device && snooped_device[0])
    {
        driverio io;
        driverio_init(&io);

        userio_xmlv1(&io.userio, io.user);
        IUUserIOGetProperties(&io.userio, io.user, snooped_device, snooped_property);

        driverio_finish(&io);
    }
}

/* tell indiserver whether we want BLOBs from the given snooped device.
 * silently ignored if given device is not already registered for snooping.
 */
void IDSnoopBLOBs(const char *snooped_device, const char *snooped_property, BLOBHandling bh)
{
    if (snooped_device && snooped_device[0])
    {
        driverio io;
        driverio_init(&io);

        userio_xmlv1(&io.userio, io.user);
        IUUserIOEnableBLOB(&io.userio, io.user, snooped_device, snooped_property, bh);

        driverio_finish(&io);
    }
}

/* crack the given INDI XML element and call driver's IS* entry points as they
 *   are recognized.
 * return 0 if ok else -1 with reason in msg[].
 * N.B. exit if getProperties does not proclaim a compatible version.
 */
int dispatch(XMLEle *root, char msg[])
{
    char *rtag = tagXMLEle(root);
    XMLEle *ep;
    int n;

    if (verbose)
        prXMLEle(stderr, root, 0);

    if (!strcmp(rtag, "getProperties"))
    {
        XMLAtt *ap, *name, *dev;
        double v;

        /* check version */
        ap = findXMLAtt(root, "version");
        if (!ap)
        {
            fprintf(stderr, "%s: getProperties missing version\n", me);
            exit(1);
        }
        v = atof(valuXMLAtt(ap));
        if (v > INDIV)
        {
            fprintf(stderr, "%s: client version %g > %g\n", me, v, INDIV);
            exit(1);
        }

        // Get device
        dev = findXMLAtt(root, "device");

        // Get property name
        name = findXMLAtt(root, "name");

        if (name && dev)
        {
            pthread_mutex_lock(&rosc_mutex);
            ROSC *prop = rosc_find(valuXMLAtt(name), valuXMLAtt(dev));
            // This is unsafe... prop may be modified concurrently here
            pthread_mutex_unlock(&rosc_mutex);

            if (prop == NULL)
                return 0;

            switch (prop->type)
            {
                /* JM 2019-07-18: Why are we using setXXX here? should be defXXX */
                case INDI_NUMBER:
                    //IDSetNumber((INumberVectorProperty *)(prop->ptr), NULL);
                    IDDefNumber((INumberVectorProperty *)(prop->ptr), NULL);
                    return 0;

                case INDI_SWITCH:
                    //IDSetSwitch((ISwitchVectorProperty *)(prop->ptr), NULL);
                    IDDefSwitch((ISwitchVectorProperty *)(prop->ptr), NULL);
                    return 0;

                case INDI_TEXT:
                    //IDSetText((ITextVectorProperty *)(prop->ptr), NULL);
                    IDDefText((ITextVectorProperty *)(prop->ptr), NULL);
                    return 0;

                case INDI_BLOB:
                    //IDSetBLOB((IBLOBVectorProperty *)(prop->ptr), NULL);
                    IDDefBLOB((IBLOBVectorProperty *)(prop->ptr), NULL);
                    return 0;
                default:
                    return 0;
            }
        }

        ISGetProperties(dev ? valuXMLAtt(dev) : NULL);
        return (0);
    }

    /* other commands might be from a snooped device.
         * we don't know here which devices are being snooped so we send
         * all remaining valid messages
         */
    if (!strcmp(rtag, "setNumberVector") || !strcmp(rtag, "setTextVector") || !strcmp(rtag, "setLightVector") ||
            !strcmp(rtag, "setSwitchVector") || !strcmp(rtag, "setBLOBVector") || !strcmp(rtag, "defNumberVector") ||
            !strcmp(rtag, "defTextVector") || !strcmp(rtag, "defLightVector") || !strcmp(rtag, "defSwitchVector") ||
            !strcmp(rtag, "defBLOBVector") || !strcmp(rtag, "message") || !strcmp(rtag, "delProperty"))
    {
        ISSnoopDevice(root);
        return (0);
    }

    char *dev, *name;
    /* pull out device and name */
    if (crackDN(root, &dev, &name, msg) < 0)
        return (-1);

    pthread_mutex_lock(&rosc_mutex);
    if (rosc_find(name, dev) == NULL)
    {
        pthread_mutex_unlock(&rosc_mutex);
        snprintf(msg, MAXRBUF, "Property %s is not defined in %s.", name, dev);
        return -1;
    }
    pthread_mutex_unlock(&rosc_mutex);

    /* ensure property is not RO */
    for (int i = 0; i < nPropCache; i++)
    {
        if (!strcmp(propCache[i].propName, name) && !strcmp(propCache[i].devName, dev))
        {
            if (propCache[i].perm == IP_RO)
            {
                snprintf(msg, MAXRBUF, "Cannot set read-only property %s", name);
                return -1;
            }
            else
                break;
        }
    }

    /* check tag in surmised decreasing order of likelyhood */

    if (!strcmp(rtag, "newNumberVector"))
    {
        static double *doubles = NULL;
        static char **names = NULL;
        static int maxn = 0;

        // Set locale to C and save previous value
        locale_char_t *orig = indi_locale_C_numeric_push();

        /* pull out each name/value pair */
        for (n = 0, ep = nextXMLEle(root, 1); ep; ep = nextXMLEle(root, 0))
        {
            if (strcmp(tagXMLEle(ep), "oneNumber") == 0)
            {
                XMLAtt *na = findXMLAtt(ep, "name");
                if (na)
                {
                    if (n >= maxn)
                    {
                        /* grow for this and another */
                        maxn = n + 1;
                        assert_mem(doubles = (double *)realloc(doubles, maxn * sizeof *doubles));
                        assert_mem(names = (char **)realloc(names, maxn * sizeof *names));
                    }
                    if (f_scansexa(pcdataXMLEle(ep), &doubles[n]) < 0)
                        IDMessage(dev, "[ERROR] %s: Bad format %s", name, pcdataXMLEle(ep));
                    else
                        names[n++] = valuXMLAtt(na);
                }
            }
        }

        // Reset locale settings to original value
        indi_locale_C_numeric_pop(orig);

        /* invoke driver if something to do, but not an error if not */
        if (n > 0)
            ISNewNumber(dev, name, doubles, names, n);
        else
            IDMessage(dev, "[ERROR] %s: newNumberVector with no valid members", name);
        return (0);
    }

    if (!strcmp(rtag, "newSwitchVector"))
    {
        static ISState *states = NULL;
        static char **names = NULL;
        static int maxn = 0;
        XMLEle *ep;

        /* pull out each name/state pair */
        for (n = 0, ep = nextXMLEle(root, 1); ep; ep = nextXMLEle(root, 0))
        {
            if (strcmp(tagXMLEle(ep), "oneSwitch") == 0)
            {
                XMLAtt *na = findXMLAtt(ep, "name");
                if (na)
                {
                    if (n >= maxn)
                    {
                        maxn = n + 1;
                        assert_mem(states = (ISState *)realloc(states, maxn * sizeof *states));
                        assert_mem(names = (char **)realloc(names, maxn * sizeof *names));
                    }
                    if (strncmp(pcdataXMLEle(ep), "On", 2) == 0)
                    {
                        states[n] = ISS_ON;
                        names[n]  = valuXMLAtt(na);
                        n++;
                    }
                    else if (strcmp(pcdataXMLEle(ep), "Off") == 0)
                    {
                        states[n] = ISS_OFF;
                        names[n]  = valuXMLAtt(na);
                        n++;
                    }
                    else
                        IDMessage(dev, "[ERROR] %s: must be On or Off: %s", name, pcdataXMLEle(ep));
                }
            }
        }

        /* invoke driver if something to do, but not an error if not */
        if (n > 0)
            ISNewSwitch(dev, name, states, names, n);
        else
            IDMessage(dev, "[ERROR] %s: newSwitchVector with no valid members", name);
        return (0);
    }

    if (!strcmp(rtag, "newTextVector"))
    {
        static char **texts = NULL;
        static char **names = NULL;
        static int maxn = 0;

        /* pull out each name/text pair */
        for (n = 0, ep = nextXMLEle(root, 1); ep; ep = nextXMLEle(root, 0))
        {
            if (strcmp(tagXMLEle(ep), "oneText") == 0)
            {
                XMLAtt *na = findXMLAtt(ep, "name");
                if (na)
                {
                    if (n >= maxn)
                    {
                        maxn = n + 1;
                        assert_mem(texts = (char **)realloc(texts, maxn * sizeof *texts));
                        assert_mem(names = (char **)realloc(names, maxn * sizeof *names));
                    }
                    texts[n] = pcdataXMLEle(ep);
                    names[n] = valuXMLAtt(na);
                    n++;
                }
            }
        }

        /* invoke driver if something to do, but not an error if not */
        if (n > 0)
            ISNewText(dev, name, texts, names, n);
        else
            IDMessage(dev, "[ERROR] %s: set with no valid members", name);
        return (0);
    }

    if (!strcmp(rtag, "newBLOBVector"))
    {
        static char **blobs = NULL;
        static char **names = NULL;
        static char **formats = NULL;
        static int *blobsizes = NULL;
        static int *sizes = NULL;
        static int maxn = 0;

        // FIXME: shared blob not supported here
        /* pull out each name/BLOB pair, decode */
        for (n = 0, ep = nextXMLEle(root, 1); ep; ep = nextXMLEle(root, 0))
        {
            if (strcmp(tagXMLEle(ep), "oneBLOB") == 0)
            {
                XMLAtt *na = findXMLAtt(ep, "name");
                XMLAtt *fa = findXMLAtt(ep, "format");
                XMLAtt *sa = findXMLAtt(ep, "size");
                XMLAtt *el = findXMLAtt(ep, "enclen");
                if (na && fa && sa)
                {
                    if (n >= maxn)
                    {
                        maxn = n + 1;
                        assert_mem(blobs = (char **)realloc(blobs, maxn * sizeof *blobs));
                        assert_mem(names = (char **)realloc(names, maxn * sizeof *names));
                        assert_mem(formats = (char **)realloc(formats, maxn * sizeof *formats));
                        assert_mem(sizes = (int *)realloc(sizes, maxn * sizeof *sizes));
                        assert_mem(blobsizes = (int *)realloc(blobsizes, maxn * sizeof *blobsizes));
                    }
                    // FIXME : here decode using shared buffer
                    int bloblen = pcdatalenXMLEle(ep);
                    // enclen is optional and not required by INDI protocol
                    if (el)
                        bloblen = atoi(valuXMLAtt(el));
                    assert_mem(blobs[n] = (char*)malloc(3 * bloblen / 4));
                    blobsizes[n] = from64tobits_fast(blobs[n], pcdataXMLEle(ep), bloblen);
                    names[n]     = valuXMLAtt(na);
                    formats[n]   = valuXMLAtt(fa);
                    sizes[n]     = atoi(valuXMLAtt(sa));
                    n++;
                }
            }
        }

        /* invoke driver if something to do, but not an error if not */
        if (n > 0)
        {
            ISNewBLOB(dev, name, sizes, blobsizes, blobs, formats, names, n);
            for (int i = 0; i < n; i++)
                free(blobs[i]);
        }
        else
            IDMessage(dev, "[ERROR] %s: newBLOBVector with no valid members", name);
        return (0);
    }

    sprintf(msg, "Unknown command: %s", rtag);
    return (1);
}

int IUReadConfig(const char *filename, const char *dev, const char *property, int silent, char errmsg[])
{
    char *rname, *rdev;
    XMLEle *root = NULL, *fproot = NULL;
    LilXML *lp = newLilXML();

    FILE *fp = IUGetConfigFP(filename, dev, "r", errmsg);

    if (fp == NULL)
        return -1;

    char whynot[MAXRBUF];
    fproot = readXMLFile(fp, lp, whynot);

    delLilXML(lp);

    if (fproot == NULL)
    {
        snprintf(errmsg, MAXRBUF, "Unable to parse config XML: %s", whynot);
        fclose(fp);
        return -1;
    }

    if (nXMLEle(fproot) > 0 && silent != 1)
        IDMessage(dev, "[INFO] Loading device configuration...");

    for (root = nextXMLEle(fproot, 1); root != NULL; root = nextXMLEle(fproot, 0))
    {
        /* pull out device and name */
        if (crackDN(root, &rdev, &rname, errmsg) < 0)
        {
            fclose(fp);
            delXMLEle(fproot);
            return -1;
        }

        // It doesn't belong to our device??
        if (strcmp(dev, rdev))
            continue;

        if ((property && !strcmp(property, rname)) || property == NULL)
        {
            dispatch(root, errmsg);
            if (property)
                break;
        }
    }

    if (nXMLEle(fproot) > 0 && silent != 1)
        IDMessage(dev, "[INFO] Device configuration applied.");

    fclose(fp);
    delXMLEle(fproot);

    return (0);
}

int IUSaveDefaultConfig(const char *source_config, const char *dest_config, const char *dev)
{
    char configFileName[MAXRBUF], configDefaultFileName[MAXRBUF];

    if (source_config)
        strncpy(configFileName, source_config, MAXRBUF);
    else
    {
        if (getenv("INDICONFIG"))
            strncpy(configFileName, getenv("INDICONFIG"), MAXRBUF);
        else
            snprintf(configFileName, MAXRBUF, "%s/.indi/%s_config.xml", getenv("HOME"), dev);
    }

    if (dest_config)
        strncpy(configDefaultFileName, dest_config, MAXRBUF);
    else if (getenv("INDICONFIG"))
        snprintf(configDefaultFileName, MAXRBUF, "%s.default", getenv("INDICONFIG"));
    else
        snprintf(configDefaultFileName, MAXRBUF, "%s/.indi/%s_config.xml.default", getenv("HOME"), dev);

    // If the default doesn't exist, create it.
    if (access(configDefaultFileName, F_OK))
    {
        FILE *fpin = fopen(configFileName, "r");
        if (fpin != NULL)
        {
            FILE *fpout = fopen(configDefaultFileName, "w");
            if (fpout != NULL)
            {
                int ch = 0;
                while ((ch = getc(fpin)) != EOF)
                    putc(ch, fpout);

                fflush(fpout);
                fclose(fpout);
            }
            fclose(fpin);

            return 0;
        }
    }
    // If default config file exists already, then no need to modify it
    else
        return 0;


    return -1;
}

int IUGetConfigOnSwitch(const ISwitchVectorProperty *property, int *index)
{
    char *rname, *rdev;
    XMLEle *root = NULL, *fproot = NULL;
    char errmsg[MAXRBUF];
    LilXML *lp = newLilXML();
    int propertyFound = 0;
    *index = -1;

    FILE *fp = IUGetConfigFP(NULL, property->device, "r", errmsg);

    if (fp == NULL)
    {
        delLilXML(lp);
        return -1;
    }

    fproot = readXMLFile(fp, lp, errmsg);

    if (fproot == NULL)
    {
        delLilXML(lp);
        fclose(fp);
        return -1;
    }

    for (root = nextXMLEle(fproot, 1); root != NULL; root = nextXMLEle(fproot, 0))
    {
        /* pull out device and name */
        if (crackDN(root, &rdev, &rname, errmsg) < 0)
        {
            fclose(fp);
            delXMLEle(fproot);
            return -1;
        }

        // It doesn't belong to our device??
        if (strcmp(property->device, rdev))
            continue;

        if (!strcmp(property->name, rname))
        {
            propertyFound = 1;
            XMLEle *oneSwitch = NULL;
            int oneSwitchIndex = 0;
            ISState oneSwitchState;
            for (oneSwitch = nextXMLEle(root, 1); oneSwitch != NULL; oneSwitch = nextXMLEle(root, 0), oneSwitchIndex++)
            {
                if (crackISState(pcdataXMLEle(oneSwitch), &oneSwitchState) == 0 && oneSwitchState == ISS_ON)
                {
                    *index = oneSwitchIndex;
                    break;
                }
            }
            break;
        }
    }

    fclose(fp);
    delXMLEle(fproot);
    delLilXML(lp);

    return (propertyFound ? 0 : -1);
}

int IUGetConfigSwitch(const char *dev, const char *property, const char *member, ISState *value)
{
    char *rname, *rdev;
    XMLEle *root = NULL, *fproot = NULL;
    char errmsg[MAXRBUF];
    LilXML *lp = newLilXML();
    int valueFound = 0;

    FILE *fp = IUGetConfigFP(NULL, dev, "r", errmsg);

    if (fp == NULL)
    {
        delLilXML(lp);
        return -1;
    }

    fproot = readXMLFile(fp, lp, errmsg);

    if (fproot == NULL)
    {
        fclose(fp);
        delLilXML(lp);
        return -1;
    }

    for (root = nextXMLEle(fproot, 1); root != NULL; root = nextXMLEle(fproot, 0))
    {
        /* pull out device and name */
        if (crackDN(root, &rdev, &rname, errmsg) < 0)
        {
            fclose(fp);
            delXMLEle(fproot);
            return -1;
        }

        // It doesn't belong to our device??
        if (strcmp(dev, rdev))
            continue;

        if ((property && !strcmp(property, rname)) || property == NULL)
        {
            XMLEle *oneSwitch = NULL;
            for (oneSwitch = nextXMLEle(root, 1); oneSwitch != NULL; oneSwitch = nextXMLEle(root, 0))
            {
                if (!strcmp(member, findXMLAttValu(oneSwitch, "name")))
                {
                    if (crackISState(pcdataXMLEle(oneSwitch), value) == 0)
                        valueFound = 1;
                    break;
                }
            }
            break;
        }
    }

    fclose(fp);
    delXMLEle(fproot);
    delLilXML(lp);

    return (valueFound == 1 ? 0 : -1);
}

int IUGetConfigOnSwitchIndex(const char *dev, const char *property, int *index)
{
    char *rname, *rdev;
    XMLEle *root = NULL, *fproot = NULL;
    char errmsg[MAXRBUF];
    LilXML *lp = newLilXML();
    int valueFound = 0;

    FILE *fp = IUGetConfigFP(NULL, dev, "r", errmsg);

    if (fp == NULL)
    {
        delLilXML(lp);
        return -1;
    }

    fproot = readXMLFile(fp, lp, errmsg);

    if (fproot == NULL)
    {
        delLilXML(lp);
        fclose(fp);
        return -1;
    }

    for (root = nextXMLEle(fproot, 1); root != NULL; root = nextXMLEle(fproot, 0))
    {
        /* pull out device and name */
        if (crackDN(root, &rdev, &rname, errmsg) < 0)
        {
            fclose(fp);
            delXMLEle(fproot);
            return -1;
        }

        // It doesn't belong to our device??
        if (strcmp(dev, rdev))
            continue;

        if ((property && !strcmp(property, rname)) || property == NULL)
        {
            XMLEle *oneSwitch = NULL;
            int currentIndex = 0;
            for (oneSwitch = nextXMLEle(root, 1); oneSwitch != NULL; oneSwitch = nextXMLEle(root, 0), currentIndex++)
            {
                ISState s = ISS_OFF;
                if (crackISState(pcdataXMLEle(oneSwitch), &s) == 0 && s == ISS_ON)
                {
                    *index = currentIndex;
                    valueFound = 1;
                    break;
                }
            }
            break;
        }
    }

    fclose(fp);
    delXMLEle(fproot);
    delLilXML(lp);

    return (valueFound == 1 ? 0 : -1);
}

int IUGetConfigOnSwitchName(const char *dev, const char *property, char *name, size_t size)
{
    char *rname, *rdev;
    XMLEle *root = NULL, *fproot = NULL;
    char errmsg[MAXRBUF];
    LilXML *lp = newLilXML();
    int found = -1;

    FILE *fp = IUGetConfigFP(NULL, dev, "r", errmsg);

    if (fp == NULL)
    {
        delLilXML(lp);
        return -1;
    }

    fproot = readXMLFile(fp, lp, errmsg);

    if (fproot == NULL)
    {
        delLilXML(lp);
        fclose(fp);
        return -1;
    }

    for (root = nextXMLEle(fproot, 1); root != NULL; root = nextXMLEle(fproot, 0))
    {
        /* pull out device and name */
        if (crackDN(root, &rdev, &rname, errmsg) < 0)
        {
            fclose(fp);
            delXMLEle(fproot);
            return -1;
        }

        // It doesn't belong to our device??
        if (strcmp(dev, rdev))
            continue;

        if ((property && !strcmp(property, rname)) || property == NULL)
        {
            XMLEle *oneSwitch = NULL;
            int currentIndex = 0;
            for (oneSwitch = nextXMLEle(root, 1); oneSwitch != NULL; oneSwitch = nextXMLEle(root, 0), currentIndex++)
            {
                ISState s = ISS_OFF;
                if (crackISState(pcdataXMLEle(oneSwitch), &s) == 0 && s == ISS_ON)
                {
                    found = 0;
                    strncpy(name, findXMLAttValu(oneSwitch, "name"), size);
                    break;
                }
            }
            break;
        }
    }

    fclose(fp);
    delXMLEle(fproot);
    delLilXML(lp);

    return found;
}

int IUGetConfigNumber(const char *dev, const char *property, const char *member, double *value)
{
    char *rname, *rdev;
    XMLEle *root = NULL, *fproot = NULL;
    char errmsg[MAXRBUF];
    LilXML *lp = newLilXML();
    int valueFound = 0;

    FILE *fp = IUGetConfigFP(NULL, dev, "r", errmsg);

    if (fp == NULL)
    {
        delLilXML(lp);
        return -1;
    }

    fproot = readXMLFile(fp, lp, errmsg);

    if (fproot == NULL)
    {
        fclose(fp);
        delLilXML(lp);
        return -1;
    }

    for (root = nextXMLEle(fproot, 1); root != NULL; root = nextXMLEle(fproot, 0))
    {
        /* pull out device and name */
        if (crackDN(root, &rdev, &rname, errmsg) < 0)
        {
            fclose(fp);
            delXMLEle(fproot);
            return -1;
        }

        // It doesn't belong to our device??
        if (strcmp(dev, rdev))
            continue;

        if ((property && !strcmp(property, rname)) || property == NULL)
        {
            XMLEle *oneNumber = NULL;
            for (oneNumber = nextXMLEle(root, 1); oneNumber != NULL; oneNumber = nextXMLEle(root, 0))
            {
                if (!strcmp(member, findXMLAttValu(oneNumber, "name")))
                {
                    *value = atof(pcdataXMLEle(oneNumber));
                    valueFound = 1;
                    break;
                }
            }
            break;
        }
    }

    fclose(fp);
    delXMLEle(fproot);
    delLilXML(lp);

    return (valueFound == 1 ? 0 : -1);
}

int IUGetConfigText(const char *dev, const char *property, const char *member, char *value, int len)
{
    char *rname, *rdev;
    XMLEle *root = NULL, *fproot = NULL;
    char errmsg[MAXRBUF];
    LilXML *lp = newLilXML();
    int valueFound = 0;

    FILE *fp = IUGetConfigFP(NULL, dev, "r", errmsg);

    if (fp == NULL)
    {
        delLilXML(lp);
        return -1;
    }

    fproot = readXMLFile(fp, lp, errmsg);

    if (fproot == NULL)
    {
        delLilXML(lp);
        fclose(fp);
        return -1;
    }

    for (root = nextXMLEle(fproot, 1); root != NULL; root = nextXMLEle(fproot, 0))
    {
        /* pull out device and name */
        if (crackDN(root, &rdev, &rname, errmsg) < 0)
        {
            fclose(fp);
            delXMLEle(fproot);
            return -1;
        }

        // It doesn't belong to our device??
        if (strcmp(dev, rdev))
            continue;

        if ((property && !strcmp(property, rname)) || property == NULL)
        {
            XMLEle *oneText = NULL;
            for (oneText = nextXMLEle(root, 1); oneText != NULL; oneText = nextXMLEle(root, 0))
            {
                if (!strcmp(member, findXMLAttValu(oneText, "name")))
                {
                    strncpy(value, pcdataXMLEle(oneText), len);
                    valueFound = 1;
                    break;
                }
            }
            break;
        }
    }

    fclose(fp);
    delXMLEle(fproot);
    delLilXML(lp);

    return (valueFound == 1 ? 0 : -1);
}

/* send client a message for a specific device or at large if !dev */
void IDMessageVA(const char *dev, const char *fmt, va_list ap)
{
    driverio io;
    driverio_init(&io);

    userio_xmlv1(&io.userio, io.user);

    IDUserIOMessageVA(&io.userio, io.user, dev, fmt, ap);

    driverio_finish(&io);
}

void IDMessage(const char *dev, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    IDMessageVA(dev, fmt, ap);
    va_end(ap);
}

int IUPurgeConfig(const char *filename, const char *dev, char errmsg[])
{
    char configFileName[MAXRBUF];
    char configDir[MAXRBUF];

    snprintf(configDir, MAXRBUF, "%s/.indi/", getenv("HOME"));

    if (filename)
        strncpy(configFileName, filename, MAXRBUF);
    else
    {
        if (getenv("INDICONFIG"))
            strncpy(configFileName, getenv("INDICONFIG"), MAXRBUF);
        else
            snprintf(configFileName, MAXRBUF, "%s%s_config.xml", configDir, dev);
    }

    if (remove(configFileName) != 0)
    {
        snprintf(errmsg, MAXRBUF, "Unable to purge configuration file %s. Error %s", configFileName, strerror(errno));
        return -1;
    }

    return 0;
}

FILE *IUGetConfigFP(const char *filename, const char *dev, const char *mode, char errmsg[])
{
    char configFileName[MAXRBUF];
    char configDir[MAXRBUF];
    struct stat st;
    FILE *fp = NULL;

    snprintf(configDir, MAXRBUF, "%s/.indi/", getenv("HOME"));

    if (filename)
        strncpy(configFileName, filename, MAXRBUF);
    else
    {
        if (getenv("INDICONFIG"))
            strncpy(configFileName, getenv("INDICONFIG"), MAXRBUF);
        else
            snprintf(configFileName, MAXRBUF, "%s%s_config.xml", configDir, dev);
    }

    if (stat(configDir, &st) != 0)
    {
        if (mkdir(configDir, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH) < 0)
        {
            snprintf(errmsg, MAXRBUF, "Unable to create config directory. Error %s: %s", configDir, strerror(errno));
            return NULL;
        }
    }

    stat(configFileName, &st);
    /* If file is owned by root and current user is NOT root then abort */
    if ( (st.st_uid == 0 && getuid() != 0) || (st.st_gid == 0 && getgid() != 0) )
    {
        strncpy(errmsg,
                "Config file is owned by root! This will lead to serious errors. To fix this, run: sudo chown -R $USER:$USER ~/.indi",
                MAXRBUF);
        return NULL;
    }

    fp = fopen(configFileName, mode);
    if (fp == NULL)
    {
        snprintf(errmsg, MAXRBUF, "Unable to open config file. Error loading file %s: %s", configFileName,
                 strerror(errno));
        return NULL;
    }

    return fp;
}

void IUSaveConfigTag(FILE *fp, int ctag, const char *dev, int silent)
{
    if (!fp)
        return;

    IUUserIOConfigTag(userio_file(), fp, ctag);

    if (silent != 1)
    {
        /* Opening tag */
        if (ctag == 0)
        {
            IDMessage(dev, "[INFO] Saving device configuration...");
        }
        /* Closing tag */
        else
        {
            IDMessage(dev, "[INFO] Device configuration saved.");
        }
    }
}

/* tell client to create a text vector property */
void IDDefTextVA(const ITextVectorProperty *tvp, const char *fmt, va_list ap)
{
    driverio io;
    driverio_init(&io);

    userio_xmlv1(&io.userio, io.user);
    IUUserIODefTextVA(&io.userio, io.user, tvp, fmt, ap);

    driverio_finish(&io);

    /* Add this property to insure proper sanity check */
    rosc_add_unique(tvp->name, tvp->device, tvp->p, tvp, INDI_TEXT);

}

void IDDefText(const ITextVectorProperty *tvp, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    IDDefTextVA(tvp, fmt, ap);
    va_end(ap);
}

/* tell client to create a new numeric vector property */
void IDDefNumberVA(const INumberVectorProperty *nvp, const char *fmt, va_list ap)
{
    driverio io;
    driverio_init(&io);

    userio_xmlv1(&io.userio, io.user);
    IUUserIODefNumberVA(&io.userio, io.user, nvp, fmt, ap);

    driverio_finish(&io);

    /* Add this property to insure proper sanity check */
    rosc_add_unique(nvp->name, nvp->device, nvp->p, nvp, INDI_NUMBER);
}

void IDDefNumber(const INumberVectorProperty *nvp, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    IDDefNumberVA(nvp, fmt, ap);
    va_end(ap);
}

/* tell client to create a new switch vector property */
void IDDefSwitchVA(const ISwitchVectorProperty *svp, const char *fmt, va_list ap)
{
    driverio io;
    driverio_init(&io);

    userio_xmlv1(&io.userio, io.user);
    IUUserIODefSwitchVA(&io.userio, io.user, svp, fmt, ap);

    driverio_finish(&io);

    /* Add this property to insure proper sanity check */
    rosc_add_unique(svp->name, svp->device, svp->p, svp, INDI_SWITCH);
}

void IDDefSwitch(const ISwitchVectorProperty *svp, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    IDDefSwitchVA(svp, fmt, ap);
    va_end(ap);
}

/* tell client to create a new lights vector property */
void IDDefLightVA(const ILightVectorProperty *lvp, const char *fmt, va_list ap)
{
    driverio io;
    driverio_init(&io);

    userio_xmlv1(&io.userio, io.user);
    IUUserIODefLightVA(&io.userio, io.user, lvp, fmt, ap);

    driverio_finish(&io);
}

void IDDefLight(const ILightVectorProperty *lvp, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    IDDefLightVA(lvp, fmt, ap);
    va_end(ap);
}

/* tell client to create a new BLOB vector property */
void IDDefBLOBVA(const IBLOBVectorProperty *bvp, const char *fmt, va_list ap)
{
    driverio io;
    driverio_init(&io);

    userio_xmlv1(&io.userio, io.user);
    IUUserIODefBLOBVA(&io.userio, io.user, bvp, fmt, ap);

    driverio_finish(&io);

    /* Add this property to insure proper sanity check */
    rosc_add_unique(bvp->name, bvp->device, bvp->p, bvp, INDI_BLOB);

}

void IDDefBLOB(const IBLOBVectorProperty *bvp, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    IDDefBLOBVA(bvp, fmt, ap);
    va_end(ap);
}

/* tell client to update an existing text vector property */
void IDSetTextVA(const ITextVectorProperty *tvp, const char *fmt, va_list ap)
{
    driverio io;
    driverio_init(&io);

    userio_xmlv1(&io.userio, io.user);
    IUUserIOSetTextVA(&io.userio, io.user, tvp, fmt, ap);

    driverio_finish(&io);
}

void IDSetText(const ITextVectorProperty *tvp, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    IDSetTextVA(tvp, fmt, ap);
    va_end(ap);
}

/* tell client to update an existing numeric vector property */
void IDSetNumberVA(const INumberVectorProperty *nvp, const char *fmt, va_list ap)
{
    driverio io;
    driverio_init(&io);

    userio_xmlv1(&io.userio, io.user);
    IUUserIOSetNumberVA(&io.userio, io.user, nvp, fmt, ap);

    driverio_finish(&io);
}

void IDSetNumber(const INumberVectorProperty *nvp, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    IDSetNumberVA(nvp, fmt, ap);
    va_end(ap);
}

/* tell client to update an existing switch vector property */
void IDSetSwitchVA(const ISwitchVectorProperty *svp, const char *fmt, va_list ap)
{
    driverio io;
    driverio_init(&io);

    userio_xmlv1(&io.userio, io.user);
    IUUserIOSetSwitchVA(&io.userio, io.user, svp, fmt, ap);

    driverio_finish(&io);
}

void IDSetSwitch(const ISwitchVectorProperty *svp, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    IDSetSwitchVA(svp, fmt, ap);
    va_end(ap);
}

/* tell client to update an existing lights vector property */
void IDSetLightVA(const ILightVectorProperty *lvp, const char *fmt, va_list ap)
{
    driverio io;
    driverio_init(&io);

    userio_xmlv1(&io.userio, io.user);
    IUUserIOSetLightVA(&io.userio, io.user, lvp, fmt, ap);

    driverio_finish(&io);
}

void IDSetLight(const ILightVectorProperty *lvp, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    IDSetLightVA(lvp, fmt, ap);
    va_end(ap);
}

static long lastBlobPingUid = 0;
#define BLOB_PING_PATTERN "SetBLOB/%ld"

/* tell client to update an existing BLOB vector property */
void IDSetBLOBVA(const IBLOBVectorProperty *bvp, const char *fmt, va_list ap)
{
    char buffer[64];

    // Wait for ack of previous blob if any
    if (lastBlobPingUid) {
        snprintf(buffer, 64, BLOB_PING_PATTERN, lastBlobPingUid);
        waitPingReply(buffer);
    }

    driverio io;
    driverio_init(&io);

    userio_xmlv1(&io.userio, io.user);
    IUUserIOSetBLOBVA(&io.userio, io.user, bvp, fmt, ap);

    // Send a new <pingReply> so next blob won't be delayed until reception of this one
    lastBlobPingUid++;
    snprintf(buffer, 64, BLOB_PING_PATTERN, lastBlobPingUid);
    IUUserIOPingRequest(&io.userio, io.user, buffer);

    driverio_finish(&io);
}

void IDSetBLOB(const IBLOBVectorProperty *bvp, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    IDSetBLOBVA(bvp, fmt, ap);
    va_end(ap);
}

/* tell client to update min/max elements of an existing number vector property */
void IUUpdateMinMax(const INumberVectorProperty *nvp)
{
    driverio io;
    driverio_init(&io);

    userio_xmlv1(&io.userio, io.user);
    IUUserIOUpdateMinMax(&io.userio, io.user, nvp);

    driverio_finish(&io);
}

/* Update property switches in accord with states and names. */
int IUUpdateSwitch(ISwitchVectorProperty *svp, ISState *states, char *names[], int n)
{
    ISwitch *so = NULL; // On switch pointer

    assert(svp != NULL && "IUUpdateSwitch SVP is NULL");

    /* store On switch pointer */
    if (svp->r == ISR_1OFMANY)
    {
        so = IUFindOnSwitch(svp);
        IUResetSwitch(svp);
    }

    for (int i = 0; i < n; i++)
    {
        ISwitch *sp = IUFindSwitch(svp, names[i]);

        if (!sp)
        {
            svp->s = IPS_IDLE;
            IDSetSwitch(svp, "Error: %s is not a member of %s (%s) property.", names[i], svp->label, svp->name);
            return -1;
        }

        sp->s = states[i];
    }

    /* Consistency checks for ISR_1OFMANY after update. */
    if (svp->r == ISR_1OFMANY)
    {
        int t_count = 0;
        for (int i = 0; i < svp->nsp; i++)
        {
            if (svp->sp[i].s == ISS_ON)
                t_count++;
        }
        if (t_count != 1)
        {
            IUResetSwitch(svp);

            // restore previous state
            if (so)
                so->s = ISS_ON;
            svp->s = IPS_IDLE;
            IDSetSwitch(svp, "Error: invalid state switch for property %s (%s). %s.", svp->label, svp->name,
                        t_count == 0 ? "No switch is on" : "Too many switches are on");
            return -1;
        }
    }

    return 0;
}

/* Update property numbers in accord with values and names */
int IUUpdateNumber(INumberVectorProperty *nvp, double values[], char *names[], int n)
{
    assert(nvp != NULL && "IUUpdateNumber NVP is NULL");

    for (int i = 0; i < n; i++)
    {
        INumber *np = IUFindNumber(nvp, names[i]);
        if (!np)
        {
            nvp->s = IPS_IDLE;
            IDSetNumber(nvp, "Error: %s is not a member of %s (%s) property.", names[i], nvp->label, nvp->name);
            return -1;
        }

        if (values[i] < np->min || values[i] > np->max)
        {
            nvp->s = IPS_ALERT;
            IDSetNumber(nvp, "Error: Invalid range for %s (%s). Valid range is from %g to %g. Requested value is %g",
                        np->label, np->name, np->min, np->max, values[i]);
            return -1;
        }
    }

    /* First loop checks for error, second loop set all values atomically*/
    for (int i = 0; i < n; i++)
    {
        INumber *np = IUFindNumber(nvp, names[i]);
        np->value = values[i];
    }

    return 0;
}

/* Update property text in accord with texts and names */
int IUUpdateText(ITextVectorProperty *tvp, char *texts[], char *names[], int n)
{
    assert(tvp != NULL && "IUUpdateText TVP is NULL");

    for (int i = 0; i < n; i++)
    {
        IText *tp = IUFindText(tvp, names[i]);
        if (!tp)
        {
            tvp->s = IPS_IDLE;
            IDSetText(tvp, "Error: %s is not a member of %s (%s) property.", names[i], tvp->label, tvp->name);
            return -1;
        }
    }

    /* First loop checks for error, second loop set all values atomically*/
    for (int i = 0; i < n; i++)
    {
        IText *tp = IUFindText(tvp, names[i]);
        IUSaveText(tp, texts[i]);
    }

    return 0;
}

/* Update property BLOB in accord with BLOBs and names */
int IUUpdateBLOB(IBLOBVectorProperty *bvp, int sizes[], int blobsizes[], char *blobs[], char *formats[], char *names[],
                 int n)
{
    assert(bvp != NULL && "IUUpdateBLOB BVP is NULL");

    for (int i = 0; i < n; i++)
    {
        IBLOB *bp = IUFindBLOB(bvp, names[i]);
        if (!bp)
        {
            bvp->s = IPS_IDLE;
            IDSetBLOB(bvp, "Error: %s is not a member of %s (%s) property.", names[i], bvp->label, bvp->name);
            return -1;
        }
    }

    /* First loop checks for error, second loop set all values atomically*/
    for (int i = 0; i < n; i++)
    {
        IBLOB *bp = IUFindBLOB(bvp, names[i]);
        IUSaveBLOB(bp, sizes[i], blobsizes[i], blobs[i], formats[i]);
    }

    return 0;
}
