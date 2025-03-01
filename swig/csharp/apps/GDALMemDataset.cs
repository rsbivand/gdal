/******************************************************************************
 *
 * Name:     VSIMem.cs
 * Project:  GDAL CSharp Interface
 * Purpose:  A sample app for demonstrating the in-memory virtual file support.
 * Author:   Tamas Szekeres, szekerest@gmail.com
 *
 ******************************************************************************
 * Copyright (c) 2013, Tamas Szekeres
 *
 * SPDX-License-Identifier: MIT
 *****************************************************************************/

using System;
using System.IO;

using System.Runtime.InteropServices;
using OSGeo.GDAL;
using System.Drawing;
using System.Drawing.Imaging;


/**

 * <p>Title: GDAL C# GDALMemDataset example.</p>
 * <p>Description: A sample app for demonstrating the in-memory dataset driver.</p>
 * @author Tamas Szekeres (szekerest@gmail.com)
 * @version 1.0
 */



/// <summary>
/// A C# based sample for demonstrating the in-memory dataset driver..
/// </summary>

class GDALMemDataset {

	public static void usage()

	{
		Console.WriteLine("usage example: GDALMemDataset [image file]");
		System.Environment.Exit(-1);
	}

	public static void Main(string[] args) {

		if (args.Length != 1) usage();

        Gdal.AllRegister();

        Bitmap bmp = new Bitmap(args[0]);

        // set up MEM driver to read bitmap data
        int bandCount = 1;
        int pixelOffset = 1;
        DataType dataType = DataType.GDT_Byte;
        switch (bmp.PixelFormat)
        {
            case PixelFormat.Format16bppGrayScale:
                dataType = DataType.GDT_Int16;
                bandCount = 1;
                pixelOffset = 2;
                break;
            case PixelFormat.Format24bppRgb:
                dataType = DataType.GDT_Byte;
                bandCount = 3;
                pixelOffset = 3;
                break;
            case PixelFormat.Format32bppArgb:
                dataType = DataType.GDT_Byte;
                bandCount = 4;
                pixelOffset = 4;
                break;
            case PixelFormat.Format48bppRgb:
                dataType = DataType.GDT_UInt16;
                bandCount = 3;
                pixelOffset = 6;
                break;
            case PixelFormat.Format64bppArgb:
                dataType = DataType.GDT_UInt16;
                bandCount = 4;
                pixelOffset = 8;
                break;
            default:
                Console.WriteLine("Invalid pixel format " + bmp.PixelFormat.ToString());
                break;
        }

        // Use GDAL raster reading methods to read the image data directly into the Bitmap
        BitmapData bitmapData = bmp.LockBits(new Rectangle(0, 0, bmp.Width, bmp.Height), ImageLockMode.ReadOnly, bmp.PixelFormat);

        int stride = bitmapData.Stride;
        IntPtr buf = bitmapData.Scan0;

        try
        {
            Driver drvmem = Gdal.GetDriverByName("MEM");
            // create a MEM dataset
            Dataset ds = drvmem.Create("", bmp.Width, bmp.Height, 0, dataType, null);
            // add bands in a reverse order
            for (int i = 1; i <= bandCount; i++)
            {
                ds.AddBand(dataType, new string[] { "DATAPOINTER=" + Convert.ToString(buf.ToInt64() + bandCount - i), "PIXELOFFSET=" + pixelOffset, "LINEOFFSET=" + stride });
            }

            // display parameters
            Console.WriteLine("Raster dataset parameters:");
            Console.WriteLine("  RasterCount: " + ds.RasterCount);
            Console.WriteLine("  RasterSize (" + ds.RasterXSize + "," + ds.RasterYSize + ")");

            // write dataset to tif file
            Driver drv = Gdal.GetDriverByName("GTiff");

            if (drv == null)
            {
                Console.WriteLine("Can't get driver.");
                System.Environment.Exit(-1);
            }

            drv.CreateCopy("sample2.tif", ds, 0, null, null, null);
        }
        catch (Exception ex)
        {
            Console.WriteLine(ex.Message);
        }
        finally
        {
            bmp.UnlockBits(bitmapData);
        }
	}
}