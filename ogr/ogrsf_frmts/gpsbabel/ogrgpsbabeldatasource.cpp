/******************************************************************************
 *
 * Project:  OpenGIS Simple Features Reference Implementation
 * Purpose:  Implements OGRGPSBabelDataSource class.
 * Author:   Even Rouault, <even dot rouault at spatialys.com>
 *
 ******************************************************************************
 * Copyright (c) 2010-2013, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "cpl_conv.h"
#include "cpl_string.h"
#include "cpl_error.h"
#include "cpl_spawn.h"
#include "ogr_gpsbabel.h"

#include <cstring>
#include <algorithm>

/************************************************************************/
/*                      OGRGPSBabelDataSource()                         */
/************************************************************************/

OGRGPSBabelDataSource::OGRGPSBabelDataSource()
{
}

/************************************************************************/
/*                     ~OGRGPSBabelDataSource()                         */
/************************************************************************/

OGRGPSBabelDataSource::~OGRGPSBabelDataSource()

{
    CPLFree(pszGPSBabelDriverName);
    CPLFree(pszFilename);

    OGRGPSBabelDataSource::CloseDependentDatasets();

    if (!osTmpFileName.empty())
        VSIUnlink(osTmpFileName.c_str());
}

/************************************************************************/
/*                     CloseDependentDatasets()                         */
/************************************************************************/

int OGRGPSBabelDataSource::CloseDependentDatasets()
{
    if (poGPXDS == nullptr)
        return FALSE;

    GDALClose(poGPXDS);
    poGPXDS = nullptr;
    return TRUE;
}

/************************************************************************/
/*                             GetArgv()                                */
/************************************************************************/

static char **GetArgv(int bExplicitFeatures, int bWaypoints, int bRoutes,
                      int bTracks, const char *pszGPSBabelDriverName,
                      const char *pszFilename)
{
    char **argv = CSLAddString(nullptr, "gpsbabel");
    if (bExplicitFeatures)
    {
        if (bWaypoints)
            argv = CSLAddString(argv, "-w");
        if (bRoutes)
            argv = CSLAddString(argv, "-r");
        if (bTracks)
            argv = CSLAddString(argv, "-t");
    }
    argv = CSLAddString(argv, "-i");
    argv = CSLAddString(argv, pszGPSBabelDriverName);
    argv = CSLAddString(argv, "-f");
    argv = CSLAddString(argv, pszFilename);
    argv = CSLAddString(argv, "-o");
    argv = CSLAddString(argv, "gpx,gpxver=1.1");
    argv = CSLAddString(argv, "-F");
    argv = CSLAddString(argv, "-");

    return argv;
}

/************************************************************************/
/*                         IsSpecialFile()                              */
/************************************************************************/

bool OGRGPSBabelDataSource::IsSpecialFile(const char *pszFilename)
{
    return STARTS_WITH(pszFilename, "/dev/") ||
           STARTS_WITH(pszFilename, "usb:") ||
           (STARTS_WITH(pszFilename, "COM") && atoi(pszFilename + 3) > 0);
}

/************************************************************************/
/*                       IsValidDriverName()                            */
/************************************************************************/

bool OGRGPSBabelDataSource::IsValidDriverName(const char *pszGPSBabelDriverName)
{
    for (int i = 0; pszGPSBabelDriverName[i] != '\0'; i++)
    {
        char ch = pszGPSBabelDriverName[i];
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
              (ch >= '0' && ch <= '9') || ch == '_' || ch == '=' || ch == '.' ||
              ch == ','))
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Invalid GPSBabel driver name");
            return false;
        }
    }
    return true;
}

/************************************************************************/
/*                                Open()                                */
/************************************************************************/

int OGRGPSBabelDataSource::Open(const char *pszDatasourceName,
                                const char *pszGPSBabelDriverNameIn,
                                char **papszOpenOptionsIn)

{
    constexpr const char *GPSBABEL_PREFIX = "GPSBABEL:";
    if (!STARTS_WITH_CI(pszDatasourceName, GPSBABEL_PREFIX))
    {
        CPLAssert(pszGPSBabelDriverNameIn);
        pszGPSBabelDriverName = CPLStrdup(pszGPSBabelDriverNameIn);
        pszFilename = CPLStrdup(pszDatasourceName);
    }
    else
    {
        if (CSLFetchNameValue(papszOpenOptionsIn, "FILENAME"))
            pszFilename =
                CPLStrdup(CSLFetchNameValue(papszOpenOptionsIn, "FILENAME"));

        if (CSLFetchNameValue(papszOpenOptionsIn, "GPSBABEL_DRIVER"))
        {
            if (pszFilename == nullptr)
            {
                CPLError(CE_Failure, CPLE_AppDefined, "Missing FILENAME");
                return FALSE;
            }

            pszGPSBabelDriverName =
                CPLStrdup(CSLFetchNameValue(papszOpenOptionsIn, "DRIVER"));

            /* A bit of validation to avoid command line injection */
            if (!IsValidDriverName(pszGPSBabelDriverName))
                return FALSE;
        }
    }

    bool bExplicitFeatures = false;
    bool bWaypoints = true;
    bool bTracks = true;
    bool bRoutes = true;

    if (pszGPSBabelDriverName == nullptr)
    {
        const char *pszDatasourceNameAfterPrefix =
            pszDatasourceName + strlen(GPSBABEL_PREFIX);
        const char *pszSep = strchr(pszDatasourceNameAfterPrefix, ':');
        if (pszSep == nullptr)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Wrong syntax. Expected GPSBabel:driver_name:file_name");
            return FALSE;
        }

        pszGPSBabelDriverName = CPLStrdup(pszDatasourceNameAfterPrefix);
        pszGPSBabelDriverName[pszSep - pszDatasourceNameAfterPrefix] = '\0';

        /* A bit of validation to avoid command line injection */
        if (!IsValidDriverName(pszGPSBabelDriverName))
            return FALSE;

        /* Parse optional features= option */
        const char *pszAfterSep = pszSep + 1;
        constexpr const char *FEATURES_EQUAL = "features=";
        if (STARTS_WITH_CI(pszAfterSep, FEATURES_EQUAL))
        {
            const char *pszAfterFeaturesEqual =
                pszAfterSep + strlen(FEATURES_EQUAL);
            const char *pszNextSep = strchr(pszAfterFeaturesEqual, ':');
            if (pszNextSep == nullptr)
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "Wrong syntax. Expected "
                         "GPSBabel:driver_name[,options]*:["
                         "features=waypoints,tracks,routes:]file_name");
                return FALSE;
            }

            char *pszFeatures = CPLStrdup(pszAfterFeaturesEqual);
            pszFeatures[pszNextSep - pszAfterFeaturesEqual] = 0;
            char **papszTokens = CSLTokenizeString(pszFeatures);
            char **papszIter = papszTokens;
            bool bErr = false;
            bExplicitFeatures = true;
            bWaypoints = false;
            bTracks = false;
            bRoutes = false;
            while (papszIter && *papszIter)
            {
                if (EQUAL(*papszIter, "waypoints"))
                    bWaypoints = true;
                else if (EQUAL(*papszIter, "tracks"))
                    bTracks = true;
                else if (EQUAL(*papszIter, "routes"))
                    bRoutes = true;
                else
                {
                    CPLError(CE_Failure, CPLE_AppDefined,
                             "Wrong value for 'features' options");
                    bErr = true;
                }
                papszIter++;
            }
            CSLDestroy(papszTokens);
            CPLFree(pszFeatures);

            if (bErr)
                return FALSE;

            pszAfterSep = pszNextSep + 1;
        }

        if (pszFilename == nullptr)
            pszFilename = CPLStrdup(pszAfterSep);
    }

    const char *pszOptionUseTempFile =
        CPLGetConfigOption("USE_TEMPFILE", nullptr);
    if (pszOptionUseTempFile && CPLTestBool(pszOptionUseTempFile))
        osTmpFileName = CPLGenerateTempFilenameSafe(nullptr);
    else
        osTmpFileName = VSIMemGenerateHiddenFilename("gpsbabel");

    bool bRet = false;
    if (IsSpecialFile(pszFilename))
    {
        /* Special file : don't try to open it */
        char **argv = GetArgv(bExplicitFeatures, bWaypoints, bRoutes, bTracks,
                              pszGPSBabelDriverName, pszFilename);
        VSILFILE *tmpfp = VSIFOpenL(osTmpFileName.c_str(), "wb");
        bRet = (CPLSpawn(argv, nullptr, tmpfp, TRUE) == 0);
        VSIFCloseL(tmpfp);
        tmpfp = nullptr;
        CSLDestroy(argv);
        argv = nullptr;
    }
    else
    {
        VSILFILE *fp = VSIFOpenL(pszFilename, "rb");
        if (fp == nullptr)
        {
            CPLError(CE_Failure, CPLE_AppDefined, "Cannot open file %s",
                     pszFilename);
            return FALSE;
        }

        char **argv = GetArgv(bExplicitFeatures, bWaypoints, bRoutes, bTracks,
                              pszGPSBabelDriverName, "-");

        VSILFILE *tmpfp = VSIFOpenL(osTmpFileName.c_str(), "wb");

        CPLPushErrorHandler(CPLQuietErrorHandler);
        bRet = (CPLSpawn(argv, fp, tmpfp, TRUE) == 0);
        CPLPopErrorHandler();

        CSLDestroy(argv);
        argv = nullptr;

        CPLErr nLastErrorType = CPLGetLastErrorType();
        CPLErrorNum nLastErrorNo = CPLGetLastErrorNo();
        CPLString osLastErrorMsg = CPLGetLastErrorMsg();

        VSIFCloseL(tmpfp);
        tmpfp = nullptr;

        VSIFCloseL(fp);
        fp = nullptr;

        if (!bRet)
        {
            if (strstr(osLastErrorMsg.c_str(),
                       "This format cannot be used in piped commands") ==
                nullptr)
            {
                CPLError(nLastErrorType, nLastErrorNo, "%s",
                         osLastErrorMsg.c_str());
            }
            else
            {
                VSIStatBuf sStatBuf;
                if (VSIStat(pszFilename, &sStatBuf) != 0)
                {
                    CPLError(CE_Failure, CPLE_NotSupported,
                             "Driver %s only supports real (non virtual) "
                             "files",
                             pszGPSBabelDriverName);
                    return FALSE;
                }

                /* Try without piping in */
                argv = GetArgv(bExplicitFeatures, bWaypoints, bRoutes, bTracks,
                               pszGPSBabelDriverName, pszFilename);
                tmpfp = VSIFOpenL(osTmpFileName.c_str(), "wb");
                bRet = (CPLSpawn(argv, nullptr, tmpfp, TRUE) == 0);
                VSIFCloseL(tmpfp);
                tmpfp = nullptr;

                CSLDestroy(argv);
                argv = nullptr;
            }
        }
    }

    if (bRet)
    {
        poGPXDS = static_cast<GDALDataset *>(GDALOpenEx(
            osTmpFileName.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr));
        if (poGPXDS)
        {
            if (bWaypoints)
            {
                OGRLayer *poLayer = poGPXDS->GetLayerByName("waypoints");
                if (poLayer != nullptr && poLayer->GetFeatureCount() != 0)
                    apoLayers[nLayers++] = poLayer;
            }

            if (bRoutes)
            {
                OGRLayer *poLayer = poGPXDS->GetLayerByName("routes");
                if (poLayer != nullptr && poLayer->GetFeatureCount() != 0)
                    apoLayers[nLayers++] = poLayer;
                poLayer = poGPXDS->GetLayerByName("route_points");
                if (poLayer != nullptr && poLayer->GetFeatureCount() != 0)
                    apoLayers[nLayers++] = poLayer;
            }

            if (bTracks)
            {
                OGRLayer *poLayer = poGPXDS->GetLayerByName("tracks");
                if (poLayer != nullptr && poLayer->GetFeatureCount() != 0)
                    apoLayers[nLayers++] = poLayer;
                poLayer = poGPXDS->GetLayerByName("track_points");
                if (poLayer != nullptr && poLayer->GetFeatureCount() != 0)
                    apoLayers[nLayers++] = poLayer;
            }
        }
    }

    return nLayers > 0;
}

/************************************************************************/
/*                              GetLayer()                              */
/************************************************************************/

OGRLayer *OGRGPSBabelDataSource::GetLayer(int iLayer)

{
    if (iLayer < 0 || iLayer >= nLayers)
        return nullptr;

    return apoLayers[iLayer];
}
