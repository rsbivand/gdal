/******************************************************************************
 *
 * Project:  GDAL Rasterlite driver
 * Purpose:  Implement GDAL Rasterlite support using OGR SQLite driver
 * Author:   Even Rouault, <even dot rouault at spatialys.com>
 *
 **********************************************************************
 * Copyright (c) 2009-2012, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "cpl_string.h"
#include "ogr_api.h"
#include "ogr_srs_api.h"
#include "memdataset.h"

#include "rasterlitedataset.h"

/************************************************************************/
/*                  RasterliteGetTileDriverOptions ()                   */
/************************************************************************/

static char **RasterliteAddTileDriverOptionsForDriver(
    CSLConstList papszOptions, char **papszTileDriverOptions,
    const char *pszOptionName, const char *pszExpectedDriverName)
{
    const char *pszVal = CSLFetchNameValue(papszOptions, pszOptionName);
    if (pszVal)
    {
        const char *pszDriverName =
            CSLFetchNameValueDef(papszOptions, "DRIVER", "GTiff");
        if (EQUAL(pszDriverName, pszExpectedDriverName))
        {
            papszTileDriverOptions =
                CSLSetNameValue(papszTileDriverOptions, pszOptionName, pszVal);
        }
        else
        {
            CPLError(CE_Warning, CPLE_NotSupported,
                     "Unexpected option '%s' for driver '%s'", pszOptionName,
                     pszDriverName);
        }
    }
    return papszTileDriverOptions;
}

char **RasterliteGetTileDriverOptions(CSLConstList papszOptions)
{
    const char *pszDriverName =
        CSLFetchNameValueDef(papszOptions, "DRIVER", "GTiff");

    char **papszTileDriverOptions = nullptr;

    const char *pszQuality = CSLFetchNameValue(papszOptions, "QUALITY");
    if (pszQuality)
    {
        if (EQUAL(pszDriverName, "GTiff"))
        {
            papszTileDriverOptions = CSLSetNameValue(
                papszTileDriverOptions, "JPEG_QUALITY", pszQuality);
        }
        else if (EQUAL(pszDriverName, "JPEG") || EQUAL(pszDriverName, "WEBP"))
        {
            papszTileDriverOptions =
                CSLSetNameValue(papszTileDriverOptions, "QUALITY", pszQuality);
        }
        else
        {
            CPLError(CE_Warning, CPLE_NotSupported,
                     "Unexpected option '%s' for driver '%s'", "QUALITY",
                     pszDriverName);
        }
    }

    papszTileDriverOptions = RasterliteAddTileDriverOptionsForDriver(
        papszOptions, papszTileDriverOptions, "COMPRESS", "GTiff");
    papszTileDriverOptions = RasterliteAddTileDriverOptionsForDriver(
        papszOptions, papszTileDriverOptions, "PHOTOMETRIC", "GTiff");

    return papszTileDriverOptions;
}

/************************************************************************/
/*                      RasterliteInsertSRID ()                         */
/************************************************************************/

static int RasterliteInsertSRID(GDALDatasetH hDS, const char *pszWKT)
{
    int nAuthorityCode = 0;
    CPLString osAuthorityName, osProjCS, osProj4;
    if (pszWKT != nullptr && strlen(pszWKT) != 0)
    {
        OGRSpatialReferenceH hSRS = OSRNewSpatialReference(pszWKT);
        if (hSRS)
        {
            OSRSetAxisMappingStrategy(hSRS, OAMS_TRADITIONAL_GIS_ORDER);

            const char *pszAuthorityName = OSRGetAuthorityName(hSRS, nullptr);
            if (pszAuthorityName)
                osAuthorityName = pszAuthorityName;

            const char *pszProjCS = OSRGetAttrValue(hSRS, "PROJCS", 0);
            if (pszProjCS)
                osProjCS = pszProjCS;

            const char *pszAuthorityCode = OSRGetAuthorityCode(hSRS, nullptr);
            if (pszAuthorityCode)
                nAuthorityCode = atoi(pszAuthorityCode);

            char *pszProj4 = nullptr;
            if (OSRExportToProj4(hSRS, &pszProj4) != OGRERR_NONE)
            {
                CPLFree(pszProj4);
                pszProj4 = CPLStrdup("");
            }
            osProj4 = pszProj4;
            CPLFree(pszProj4);
        }
        OSRDestroySpatialReference(hSRS);
    }

    CPLString osSQL;
    int nSRSId = -1;
    if (nAuthorityCode != 0 && !osAuthorityName.empty())
    {
        osSQL.Printf("SELECT srid FROM spatial_ref_sys WHERE auth_srid = %d",
                     nAuthorityCode);
        OGRLayerH hLyr =
            GDALDatasetExecuteSQL(hDS, osSQL.c_str(), nullptr, nullptr);
        if (hLyr == nullptr)
        {
            nSRSId = nAuthorityCode;

            if (!osProjCS.empty())
                osSQL.Printf(
                    "INSERT INTO spatial_ref_sys "
                    "(srid, auth_name, auth_srid, ref_sys_name, proj4text) "
                    "VALUES (%d, '%s', '%d', '%s', '%s')",
                    nSRSId, osAuthorityName.c_str(), nAuthorityCode,
                    osProjCS.c_str(), osProj4.c_str());
            else
                osSQL.Printf("INSERT INTO spatial_ref_sys "
                             "(srid, auth_name, auth_srid, proj4text) "
                             "VALUES (%d, '%s', '%d', '%s')",
                             nSRSId, osAuthorityName.c_str(), nAuthorityCode,
                             osProj4.c_str());

            GDALDatasetExecuteSQL(hDS, osSQL.c_str(), nullptr, nullptr);
        }
        else
        {
            OGRFeatureH hFeat = OGR_L_GetNextFeature(hLyr);
            if (hFeat)
            {
                nSRSId = OGR_F_GetFieldAsInteger(hFeat, 0);
                OGR_F_Destroy(hFeat);
            }
            GDALDatasetReleaseResultSet(hDS, hLyr);
        }
    }

    return nSRSId;
}

/************************************************************************/
/*                     RasterliteCreateTables ()                        */
/************************************************************************/

static GDALDatasetH RasterliteCreateTables(GDALDatasetH hDS,
                                           const char *pszTableName, int nSRSId,
                                           int bWipeExistingData)
{
    CPLString osSQL;

    const CPLString osDBName = GDALGetDescription(hDS);

    CPLString osRasterLayer;
    osRasterLayer.Printf("%s_rasters", pszTableName);

    CPLString osMetadataLayer;
    osMetadataLayer.Printf("%s_metadata", pszTableName);

    OGRLayerH hLyr;

    if (GDALDatasetGetLayerByName(hDS, osRasterLayer.c_str()) == nullptr)
    {
        /* --------------------------------------------------------------------
         */
        /*      The table don't exist. Create them */
        /* --------------------------------------------------------------------
         */

        /* Create _rasters table */
        osSQL.Printf("CREATE TABLE \"%s\" ("
                     "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT,"
                     "raster BLOB NOT NULL)",
                     osRasterLayer.c_str());
        GDALDatasetExecuteSQL(hDS, osSQL.c_str(), nullptr, nullptr);

        /* Create _metadata table */
        osSQL.Printf("CREATE TABLE \"%s\" ("
                     "id INTEGER NOT NULL PRIMARY KEY,"
                     "source_name TEXT NOT NULL,"
                     "tile_id INTEGER NOT NULL,"
                     "width INTEGER NOT NULL,"
                     "height INTEGER NOT NULL,"
                     "pixel_x_size DOUBLE NOT NULL,"
                     "pixel_y_size DOUBLE NOT NULL)",
                     osMetadataLayer.c_str());
        GDALDatasetExecuteSQL(hDS, osSQL.c_str(), nullptr, nullptr);

        /* Add geometry column to _metadata table */
        osSQL.Printf(
            "SELECT AddGeometryColumn('%s', 'geometry', %d, 'POLYGON', 2)",
            osMetadataLayer.c_str(), nSRSId);
        if ((hLyr = GDALDatasetExecuteSQL(hDS, osSQL.c_str(), nullptr,
                                          nullptr)) == nullptr)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Check that the OGR SQLite driver has Spatialite support");
            GDALClose(hDS);
            return nullptr;
        }
        GDALDatasetReleaseResultSet(hDS, hLyr);

        /* Create spatial index on _metadata table */
        osSQL.Printf("SELECT CreateSpatialIndex('%s', 'geometry')",
                     osMetadataLayer.c_str());
        if ((hLyr = GDALDatasetExecuteSQL(hDS, osSQL.c_str(), nullptr,
                                          nullptr)) == nullptr)
        {
            GDALClose(hDS);
            return nullptr;
        }
        GDALDatasetReleaseResultSet(hDS, hLyr);

        /* Create statistics tables */
        osSQL.Printf("SELECT UpdateLayerStatistics()");
        CPLPushErrorHandler(CPLQuietErrorHandler);
        hLyr = GDALDatasetExecuteSQL(hDS, osSQL.c_str(), nullptr, nullptr);
        CPLPopErrorHandler();
        GDALDatasetReleaseResultSet(hDS, hLyr);

        /* Re-open the DB to take into account the new tables*/
        GDALClose(hDS);

        hDS = RasterliteOpenSQLiteDB(osDBName.c_str(), GA_Update);
    }
    else
    {
        /* Check that the existing SRS is consistent with the one of the new */
        /* data to be inserted */
        osSQL.Printf(
            "SELECT srid FROM geometry_columns WHERE f_table_name = '%s'",
            osMetadataLayer.c_str());
        hLyr = GDALDatasetExecuteSQL(hDS, osSQL.c_str(), nullptr, nullptr);
        if (hLyr)
        {
            int nExistingSRID = -1;
            OGRFeatureH hFeat = OGR_L_GetNextFeature(hLyr);
            if (hFeat)
            {
                nExistingSRID = OGR_F_GetFieldAsInteger(hFeat, 0);
                OGR_F_Destroy(hFeat);
            }
            GDALDatasetReleaseResultSet(hDS, hLyr);

            if (nExistingSRID != nSRSId)
            {
                if (bWipeExistingData)
                {
                    osSQL.Printf("UPDATE geometry_columns SET srid = %d "
                                 "WHERE f_table_name = \"%s\"",
                                 nSRSId, osMetadataLayer.c_str());
                    GDALDatasetExecuteSQL(hDS, osSQL.c_str(), nullptr, nullptr);

                    /* Re-open the DB to take into account the change of SRS */
                    GDALClose(hDS);

                    hDS = RasterliteOpenSQLiteDB(osDBName.c_str(), GA_Update);
                }
                else
                {
                    CPLError(CE_Failure, CPLE_NotSupported,
                             "New data has not the same SRS as existing data");
                    GDALClose(hDS);
                    return nullptr;
                }
            }
        }

        if (bWipeExistingData)
        {
            osSQL.Printf("DELETE FROM \"%s\"", osRasterLayer.c_str());
            GDALDatasetExecuteSQL(hDS, osSQL.c_str(), nullptr, nullptr);

            osSQL.Printf("DELETE FROM \"%s\"", osMetadataLayer.c_str());
            GDALDatasetExecuteSQL(hDS, osSQL.c_str(), nullptr, nullptr);
        }
    }

    return hDS;
}

/************************************************************************/
/*                       RasterliteCreateCopy ()                        */
/************************************************************************/

GDALDataset *RasterliteCreateCopy(const char *pszFilename, GDALDataset *poSrcDS,
                                  CPL_UNUSED int bStrict, char **papszOptions,
                                  GDALProgressFunc pfnProgress,
                                  void *pProgressData)
{
    const int nBands = poSrcDS->GetRasterCount();
    if (nBands == 0)
    {
        CPLError(CE_Failure, CPLE_NotSupported, "nBands == 0");
        return nullptr;
    }

    const char *pszDriverName =
        CSLFetchNameValueDef(papszOptions, "DRIVER", "GTiff");
    if (EQUAL(pszDriverName, "MEM") || EQUAL(pszDriverName, "VRT"))
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "GDAL %s driver cannot be used as underlying driver",
                 pszDriverName);
        return nullptr;
    }

    GDALDriverH hTileDriver = GDALGetDriverByName(pszDriverName);
    if (hTileDriver == nullptr)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Cannot load GDAL %s driver",
                 pszDriverName);
        return nullptr;
    }

    GDALDriverH hMemDriver = GDALGetDriverByName("MEM");
    if (hMemDriver == nullptr)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Cannot load GDAL MEM driver");
        return nullptr;
    }

    const int nXSize = GDALGetRasterXSize(poSrcDS);
    const int nYSize = GDALGetRasterYSize(poSrcDS);

    double adfGeoTransform[6];
    if (poSrcDS->GetGeoTransform(adfGeoTransform) != CE_None)
    {
        adfGeoTransform[0] = 0;
        adfGeoTransform[1] = 1;
        adfGeoTransform[2] = 0;
        adfGeoTransform[3] = 0;
        adfGeoTransform[4] = 0;
        adfGeoTransform[5] = -1;
    }
    else if (adfGeoTransform[2] != 0.0 || adfGeoTransform[4] != 0.0)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Cannot use geotransform with rotational terms");
        return nullptr;
    }

    const bool bTiled =
        CPLTestBool(CSLFetchNameValueDef(papszOptions, "TILED", "YES"));
    int nBlockXSize, nBlockYSize;
    if (bTiled)
    {
        nBlockXSize =
            atoi(CSLFetchNameValueDef(papszOptions, "BLOCKXSIZE", "256"));
        nBlockYSize =
            atoi(CSLFetchNameValueDef(papszOptions, "BLOCKYSIZE", "256"));
        if (nBlockXSize < 64)
            nBlockXSize = 64;
        else if (nBlockXSize > 4096)
            nBlockXSize = 4096;
        if (nBlockYSize < 64)
            nBlockYSize = 64;
        else if (nBlockYSize > 4096)
            nBlockYSize = 4096;
    }
    else
    {
        nBlockXSize = nXSize;
        nBlockYSize = nYSize;
    }

    /* -------------------------------------------------------------------- */
    /*      Analyze arguments                                               */
    /* -------------------------------------------------------------------- */

    /* Skip optional RASTERLITE: prefix */
    const char *pszFilenameWithoutPrefix = pszFilename;
    if (STARTS_WITH_CI(pszFilename, "RASTERLITE:"))
        pszFilenameWithoutPrefix += 11;

    char **papszTokens =
        CSLTokenizeStringComplex(pszFilenameWithoutPrefix, ",", FALSE, FALSE);
    const int nTokens = CSLCount(papszTokens);
    CPLString osDBName;
    CPLString osTableName;
    if (nTokens == 0)
    {
        osDBName = pszFilenameWithoutPrefix;
        osTableName = CPLGetBasenameSafe(pszFilenameWithoutPrefix);
    }
    else
    {
        osDBName = papszTokens[0];

        int i;
        for (i = 1; i < nTokens; i++)
        {
            if (STARTS_WITH_CI(papszTokens[i], "table="))
                osTableName = papszTokens[i] + 6;
            else
            {
                CPLError(CE_Warning, CPLE_AppDefined, "Invalid option : %s",
                         papszTokens[i]);
            }
        }
    }

    CSLDestroy(papszTokens);
    papszTokens = nullptr;

    VSIStatBuf sBuf;
    const bool bExists = (VSIStat(osDBName.c_str(), &sBuf) == 0);

    if (osTableName.empty())
    {
        if (bExists)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Database already exists. Explicit table name must be "
                     "specified");
            return nullptr;
        }
        osTableName = CPLGetBasenameSafe(osDBName.c_str());
    }

    CPLString osRasterLayer;
    osRasterLayer.Printf("%s_rasters", osTableName.c_str());

    CPLString osMetadataLayer;
    osMetadataLayer.Printf("%s_metadata", osTableName.c_str());

    /* -------------------------------------------------------------------- */
    /*      Create or open the SQLite DB                                    */
    /* -------------------------------------------------------------------- */

    GDALDriverH hSQLiteDriver = GDALGetDriverByName("SQLite");
    if (hSQLiteDriver == nullptr)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Cannot load OGR SQLite driver");
        return nullptr;
    }

    GDALDatasetH hDS;

    if (!bExists)
    {
        char **papszOGROptions = CSLAddString(nullptr, "SPATIALITE=YES");
        hDS = GDALCreate(hSQLiteDriver, osDBName.c_str(), 0, 0, 0, GDT_Unknown,
                         papszOGROptions);
        CSLDestroy(papszOGROptions);
    }
    else
    {
        hDS = RasterliteOpenSQLiteDB(osDBName.c_str(), GA_Update);
    }

    if (hDS == nullptr)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Cannot load or create SQLite database");
        return nullptr;
    }

    CPLString osSQL;

    /* -------------------------------------------------------------------- */
    /*      Get the SRID for the SRS                                        */
    /* -------------------------------------------------------------------- */
    int nSRSId = RasterliteInsertSRID(hDS, poSrcDS->GetProjectionRef());

    /* -------------------------------------------------------------------- */
    /*      Create or wipe existing tables                                  */
    /* -------------------------------------------------------------------- */
    const int bWipeExistingData =
        CPLTestBool(CSLFetchNameValueDef(papszOptions, "WIPE", "NO"));

    hDS = RasterliteCreateTables(hDS, osTableName.c_str(), nSRSId,
                                 bWipeExistingData);
    if (hDS == nullptr)
        return nullptr;

    OGRLayerH hRasterLayer =
        GDALDatasetGetLayerByName(hDS, osRasterLayer.c_str());
    OGRLayerH hMetadataLayer =
        GDALDatasetGetLayerByName(hDS, osMetadataLayer.c_str());
    if (hRasterLayer == nullptr || hMetadataLayer == nullptr)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Cannot find metadata and/or raster tables");
        GDALClose(hDS);
        return nullptr;
    }

    /* -------------------------------------------------------------------- */
    /*      Check if there is overlapping data and warn the user            */
    /* -------------------------------------------------------------------- */
    double minx = adfGeoTransform[0];
    double maxx = adfGeoTransform[0] + nXSize * adfGeoTransform[1];
    double maxy = adfGeoTransform[3];
    double miny = adfGeoTransform[3] + nYSize * adfGeoTransform[5];

    osSQL.Printf(
        "SELECT COUNT(geometry) FROM \"%s\" "
        "WHERE rowid IN "
        "(SELECT pkid FROM \"idx_%s_metadata_geometry\" "
        "WHERE %s) AND %s",
        osMetadataLayer.c_str(), osTableName.c_str(),
        RasterliteGetSpatialFilterCond(minx, miny, maxx, maxy).c_str(),
        RasterliteGetPixelSizeCond(adfGeoTransform[1], -adfGeoTransform[5])
            .c_str());

    int nOverlappingGeoms = 0;
    OGRLayerH hCountLyr =
        GDALDatasetExecuteSQL(hDS, osSQL.c_str(), nullptr, nullptr);
    if (hCountLyr)
    {
        OGRFeatureH hFeat = OGR_L_GetNextFeature(hCountLyr);
        if (hFeat)
        {
            nOverlappingGeoms = OGR_F_GetFieldAsInteger(hFeat, 0);
            OGR_F_Destroy(hFeat);
        }
        GDALDatasetReleaseResultSet(hDS, hCountLyr);
    }

    if (nOverlappingGeoms != 0)
    {
        CPLError(CE_Warning, CPLE_AppDefined,
                 "Raster tiles already exist in the %s table within "
                 "the extent of the data to be inserted in",
                 osTableName.c_str());
    }

    /* -------------------------------------------------------------------- */
    /*      Iterate over blocks to add data into raster and metadata tables */
    /* -------------------------------------------------------------------- */
    int nXBlocks = (nXSize + nBlockXSize - 1) / nBlockXSize;
    int nYBlocks = (nYSize + nBlockYSize - 1) / nBlockYSize;

    GDALDataType eDataType = poSrcDS->GetRasterBand(1)->GetRasterDataType();
    int nDataTypeSize = GDALGetDataTypeSize(eDataType) / 8;
    GByte *pabyMEMDSBuffer = reinterpret_cast<GByte *>(VSIMalloc3(
        nBlockXSize, nBlockYSize, cpl::fits_on<int>(nBands * nDataTypeSize)));
    if (pabyMEMDSBuffer == nullptr)
    {
        GDALClose(hDS);
        return nullptr;
    }

    const CPLString osTempFileName(
        VSIMemGenerateHiddenFilename("rasterlite_tile"));

    int nTileId = 0;
    int nBlocks = 0;
    int nTotalBlocks = nXBlocks * nYBlocks;

    char **papszTileDriverOptions =
        RasterliteGetTileDriverOptions(papszOptions);

    GDALDatasetExecuteSQL(hDS, "BEGIN", nullptr, nullptr);

    CPLErr eErr = CE_None;
    for (int nBlockYOff = 0; eErr == CE_None && nBlockYOff < nYBlocks;
         nBlockYOff++)
    {
        for (int nBlockXOff = 0; eErr == CE_None && nBlockXOff < nXBlocks;
             nBlockXOff++)
        {
            /* --------------------------------------------------------------------
             */
            /*      Create in-memory tile */
            /* --------------------------------------------------------------------
             */
            int nReqXSize = nBlockXSize;
            int nReqYSize = nBlockYSize;
            if ((nBlockXOff + 1) * nBlockXSize > nXSize)
                nReqXSize = nXSize - nBlockXOff * nBlockXSize;
            if ((nBlockYOff + 1) * nBlockYSize > nYSize)
                nReqYSize = nYSize - nBlockYOff * nBlockYSize;

            eErr = poSrcDS->RasterIO(
                GF_Read, nBlockXOff * nBlockXSize, nBlockYOff * nBlockYSize,
                nReqXSize, nReqYSize, pabyMEMDSBuffer, nReqXSize, nReqYSize,
                eDataType, nBands, nullptr, 0, 0, 0, nullptr);
            if (eErr != CE_None)
            {
                break;
            }

            auto poMEMDS = std::unique_ptr<MEMDataset>(MEMDataset::Create(
                "", nReqXSize, nReqYSize, 0, eDataType, nullptr));
            for (int iBand = 0; iBand < nBands; iBand++)
            {
                auto hBand = MEMCreateRasterBandEx(
                    poMEMDS.get(), iBand + 1,
                    pabyMEMDSBuffer +
                        iBand * nDataTypeSize * nReqXSize * nReqYSize,
                    eDataType, 0, 0, false);
                poMEMDS->AddMEMBand(hBand);
            }

            GDALDatasetH hOutDS = GDALCreateCopy(
                hTileDriver, osTempFileName.c_str(), poMEMDS.get(), FALSE,
                papszTileDriverOptions, nullptr, nullptr);

            if (!hOutDS)
            {
                eErr = CE_Failure;
                break;
            }
            GDALClose(hOutDS);

            /* --------------------------------------------------------------------
             */
            /*      Insert new entry into raster table */
            /* --------------------------------------------------------------------
             */
            vsi_l_offset nDataLength = 0;
            GByte *pabyData = VSIGetMemFileBuffer(osTempFileName.c_str(),
                                                  &nDataLength, FALSE);

            OGRFeatureH hFeat = OGR_F_Create(OGR_L_GetLayerDefn(hRasterLayer));
            OGR_F_SetFieldBinary(hFeat, 0, static_cast<int>(nDataLength),
                                 pabyData);

            if (OGR_L_CreateFeature(hRasterLayer, hFeat) != OGRERR_NONE)
                eErr = CE_Failure;
            /* Query raster ID to set it as the ID of the associated metadata */
            int nRasterID = static_cast<int>(OGR_F_GetFID(hFeat));

            OGR_F_Destroy(hFeat);

            VSIUnlink(osTempFileName.c_str());
            if (eErr == CE_Failure)
                break;

            /* --------------------------------------------------------------------
             */
            /*      Insert new entry into metadata table */
            /* --------------------------------------------------------------------
             */

            hFeat = OGR_F_Create(OGR_L_GetLayerDefn(hMetadataLayer));
            OGR_F_SetFID(hFeat, nRasterID);
            OGR_F_SetFieldString(hFeat, 0, GDALGetDescription(poSrcDS));
            OGR_F_SetFieldInteger(hFeat, 1, nTileId++);
            OGR_F_SetFieldInteger(hFeat, 2, nReqXSize);
            OGR_F_SetFieldInteger(hFeat, 3, nReqYSize);
            OGR_F_SetFieldDouble(hFeat, 4, adfGeoTransform[1]);
            OGR_F_SetFieldDouble(hFeat, 5, -adfGeoTransform[5]);

            minx = adfGeoTransform[0] +
                   (nBlockXSize * nBlockXOff) * adfGeoTransform[1];
            maxx = adfGeoTransform[0] +
                   (nBlockXSize * nBlockXOff + nReqXSize) * adfGeoTransform[1];
            maxy = adfGeoTransform[3] +
                   (nBlockYSize * nBlockYOff) * adfGeoTransform[5];
            miny = adfGeoTransform[3] +
                   (nBlockYSize * nBlockYOff + nReqYSize) * adfGeoTransform[5];

            OGRGeometryH hRectangle = OGR_G_CreateGeometry(wkbPolygon);
            OGRGeometryH hLinearRing = OGR_G_CreateGeometry(wkbLinearRing);
            OGR_G_AddPoint_2D(hLinearRing, minx, miny);
            OGR_G_AddPoint_2D(hLinearRing, minx, maxy);
            OGR_G_AddPoint_2D(hLinearRing, maxx, maxy);
            OGR_G_AddPoint_2D(hLinearRing, maxx, miny);
            OGR_G_AddPoint_2D(hLinearRing, minx, miny);
            OGR_G_AddGeometryDirectly(hRectangle, hLinearRing);

            OGR_F_SetGeometryDirectly(hFeat, hRectangle);

            if (OGR_L_CreateFeature(hMetadataLayer, hFeat) != OGRERR_NONE)
                eErr = CE_Failure;
            OGR_F_Destroy(hFeat);

            nBlocks++;
            if (pfnProgress && !pfnProgress(1.0 * nBlocks / nTotalBlocks,
                                            nullptr, pProgressData))
                eErr = CE_Failure;
        }
    }

    VSIUnlink(osTempFileName);
    VSIUnlink((osTempFileName + ".aux.xml").c_str());

    if (eErr == CE_None)
        GDALDatasetExecuteSQL(hDS, "COMMIT", nullptr, nullptr);
    else
        GDALDatasetExecuteSQL(hDS, "ROLLBACK", nullptr, nullptr);

    CSLDestroy(papszTileDriverOptions);

    VSIFree(pabyMEMDSBuffer);

    GDALClose(hDS);

    if (eErr == CE_Failure)
        return nullptr;

    return GDALDataset::FromHandle(GDALOpen(pszFilename, GA_Update));
}

/************************************************************************/
/*                         RasterliteDelete ()                          */
/************************************************************************/

CPLErr RasterliteDelete(CPL_UNUSED const char *pszFilename)
{
    return CE_None;
}
