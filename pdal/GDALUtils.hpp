/******************************************************************************
* Copyright (c) 2011, Michael P. Gerlek (mpg@flaxen.com)
*
* All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following
* conditions are met:
*
*     * Redistributions of source code must retain the above copyright
*       notice, this list of conditions and the following disclaimer.
*     * Redistributions in binary form must reproduce the above copyright
*       notice, this list of conditions and the following disclaimer in
*       the documentation and/or other materials provided
*       with the distribution.
*     * Neither the name of Hobu, Inc. or Flaxen Geo Consulting nor the
*       names of its contributors may be used to endorse or promote
*       products derived from this software without specific prior
*       written permission.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
* "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
* LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
* FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
* COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
* INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
* BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
* OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
* AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
* OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
* OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
* OF SUCH DAMAGE.
****************************************************************************/

#pragma once

#include <array>
#include <functional>
#include <mutex>
#include <sstream>
#include <vector>

#include <pdal/pdal_internal.hpp>
#include <pdal/Dimension.hpp>
#include <pdal/Log.hpp>
#include <pdal/SpatialReference.hpp>
#include <pdal/util/Bounds.hpp>

#include <cpl_conv.h>
#include <gdal_priv.h>
#include <ogr_api.h>
#include <ogr_geometry.h>
#include <ogr_srs_api.h>

class OGRSpatialReference;
class OGRGeometry;

namespace pdal
{

namespace gdal
{

template<typename ITER>
using ITER_VAL = typename std::iterator_traits<ITER>::value_type;

PDAL_DLL void registerDrivers();
PDAL_DLL void unregisterDrivers();
PDAL_DLL bool reprojectBounds(BOX3D& box, const std::string& srcSrs,
    const std::string& dstSrs);
PDAL_DLL bool reprojectBounds(BOX2D& box, const std::string& srcSrs,
    const std::string& dstSrs);
PDAL_DLL bool reproject(double& x, double& y, double& z,
    const std::string& srcSrs, const std::string& dstSrs);
PDAL_DLL std::string lastError();

typedef std::shared_ptr<void> RefPtr;

class SpatialRef
{
public:
    SpatialRef()
        { newRef(OSRNewSpatialReference("")); }
    SpatialRef(const std::string& srs)
    {
        newRef(OSRNewSpatialReference(""));
        if (OSRSetFromUserInput(get(), srs.data()) != OGRERR_NONE)
            m_ref.reset();
    }

    void setFromLayer(OGRLayerH layer)
    {
        if (layer)
        {
            OGRSpatialReferenceH s = OGR_L_GetSpatialRef(layer);
            if (s)
            {
                OGRSpatialReferenceH clone = OSRClone(s);
                newRef(clone);
            }
        }
    }
    operator bool () const
        { return m_ref.get() != NULL; }
    OGRSpatialReferenceH get() const
        { return m_ref.get(); }
    std::string wkt() const
    {
        std::string output;

        if (m_ref.get())
        {
            char *pszWKT = NULL;
            OSRExportToWkt(m_ref.get(), &pszWKT);
            bool valid = (bool)*pszWKT;
            output = pszWKT;
            CPLFree(pszWKT);
        }
        return output;
    }

    bool empty() const
    {
        return wkt().empty();
    }

private:
    void newRef(void *v)
    {
        m_ref = RefPtr(v, [](void* t){ OSRDestroySpatialReference(t); } );
    }

    RefPtr m_ref;
};

class Geometry
{
public:
    Geometry()
        {}
    Geometry(const std::string& wkt, const SpatialRef& srs)
    {
        OGRGeometryH geom;

        char *p_wkt = const_cast<char *>(wkt.data());
        OGRSpatialReferenceH ref = srs.get();
        if (srs.empty())
        {
            ref = NULL;
        }
        bool isJson = wkt.find("{") != wkt.npos ||
                      wkt.find("}") != wkt.npos;

        if (!isJson)
        {
            OGRErr err = OGR_G_CreateFromWkt(&p_wkt, ref, &geom);
            if (err != OGRERR_NONE)
            {
                std::cout << "wkt: " << wkt << std::endl;
                std::ostringstream oss;
                oss << "unable to construct OGR Geometry";
                oss << " '" << CPLGetLastErrorMsg() << "'";
                throw pdal::pdal_error(oss.str());
            }
        }
        else
        {
            // Assume it is GeoJSON and try constructing from that
            geom = OGR_G_CreateGeometryFromJson(p_wkt);

            if (!geom)
                throw pdal_error("Unable to create geometry from "
                    "input GeoJSON");

            OGR_G_AssignSpatialReference(geom, ref);
        }

        newRef(geom);
    }

    operator bool () const
        { return get() != NULL; }
    OGRGeometryH get() const
        { return m_ref.get(); }

    void transform(const SpatialRef& out_srs)
    {
        OGR_G_TransformTo(m_ref.get(), out_srs.get());
    }

    std::string wkt() const
    {
        char* p_wkt = 0;
        OGRErr err = OGR_G_ExportToWkt(m_ref.get(), &p_wkt);
        return std::string(p_wkt);
    }

    void setFromGeometry(OGRGeometryH geom)
        {
            if (geom)
                newRef(OGR_G_Clone(geom));
        }

private:
    void newRef(void *v)
    {
        m_ref = RefPtr(v, [](void* t){ OGR_G_DestroyGeometry(t); } );
    }
    RefPtr m_ref;
};


// This is a little confusing because we have a singleton error handler with
// a single log pointer, but we set the log pointer/debug state as if we
// were taking advantage of GDAL's thread-specific error handing.
//
// We lock the log/debug so that it doesn't
// get changed while another thread is using or setting.
class PDAL_DLL ErrorHandler
{
public:
    /**
      Get the singleton error handler.

      \return  Reference to the error handler.
    */
    static ErrorHandler& getGlobalErrorHandler();

    /**
      Set the log and debug state of the error handler.  This is
      a convenience and is equivalent to calling setLog() and setDebug().

      \param log  Log to write to.
      \param doDebug  Debug state of the error handler.
    */
    void set(LogPtr log, bool doDebug);

    /**
      Set the log to which error/debug messages should be written.

      \param log  Log to write to.
    */
    void setLog(LogPtr log);

    /**
      Set the debug state of the error handler.  Setting to true will also
      set the environment variable CPL_DEBUG to "ON".  This will force GDAL
      to emit debug error messages which will be logged by this handler.

      \param doDebug  Whether we're setting or clearing the debug state.
    */
    void setDebug(bool doDebug);

    /**
      Get the last error and clear the error last error value.

      \return  The last error number.
    */
    int errorNum();

    static void CPL_STDCALL trampoline(::CPLErr code, int num, char const* msg)
    {
        ErrorHandler::getGlobalErrorHandler().handle(code, num, msg);
    }

    ErrorHandler();

private:
    void handle(::CPLErr level, int num, const char *msg);

private:
    std::mutex m_mutex;
    bool m_debug;
    pdal::LogPtr m_log;
    int m_errorNum;
    bool m_cplSet;
};

class ErrorHandlerSuspender
{
public:
    ErrorHandlerSuspender()
        { CPLPushErrorHandler(CPLQuietErrorHandler); }
    ~ErrorHandlerSuspender()
        { (void)CPLPopErrorHandler(); }
};

enum class GDALError
{
    None,
    NotOpen,
    CantOpen,
    NoData,
    InvalidBand,
    BadBand,
    NoTransform,
    NotInvertible,
    CantReadBlock,
    InvalidDriver,
    DriverNotFound,
    CantCreate,
    InvalidOption,
    CantWriteBlock,
    InvalidType
};

struct InvalidBand {};
struct BadBand {};
struct CantReadBlock {};
struct CantWriteBlock
{
    CantWriteBlock()
    {}

    CantWriteBlock(const std::string& w) : what(w)
    {}

    std::string what;
};

class Raster;

/*
  Slight abstraction of a GDAL raster band.
*/
template<typename T>
class Band
{
friend class Raster;

private:
    GDALDataset *m_ds;                  /// Dataset handle
    int m_bandNum;                      /// Band number. 1-indexed.
    double m_dstNoData;                 /// Output no data value.
    GDALRasterBand *m_band;             /// Band handle
    size_t m_xTotalSize, m_yTotalSize;  /// Total size (x and y) of the raster
    size_t m_xBlockSize, m_yBlockSize;  /// Size (x and y) of blocks
    size_t m_xBlockCnt, m_yBlockCnt;    /// Number of blocks in each direction
    std::vector<T> m_buf;               /// Block read buffer.
    std::string m_name;              /// Band name.

    /**
      Create an object for reading a band of a GDAL dataset.

      \param ds  GDAL dataset handle.
      \param dstNoData  The no data value to be used when writing the band.
      \param bandNum  Band number (1-indexed).
      \param name  Name of the raster band.
    */
    Band(GDALDataset *ds, int bandNum, double dstNoData = -9999.0,
            const std::string& name = "") :
        m_ds(ds), m_bandNum(bandNum), m_dstNoData(dstNoData),
        m_xBlockSize(0), m_yBlockSize(0)
    {
        m_band = m_ds->GetRasterBand(m_bandNum);
        if (!m_band)
            throw InvalidBand();

        if (name.size())
        {
            m_band->SetDescription(name.data());
            // We don't care about offset, but this sets the flag to indicate
            // that the metadata has changed.
            m_band->SetOffset(m_band->GetOffset(NULL) + .00001);
            m_band->SetOffset(m_band->GetOffset(NULL) - .00001);
        }

        int xTotalSize = m_band->GetXSize();
        int yTotalSize = m_band->GetYSize();

        int xBlockSize, yBlockSize;
        m_band->GetBlockSize(&xBlockSize, &yBlockSize);
        if (xBlockSize <= 0 || yBlockSize <= 0 ||
            xTotalSize <= 0 || yTotalSize <= 0)
            throw BadBand();

        m_xTotalSize = (size_t)xTotalSize;
        m_yTotalSize = (size_t)yTotalSize;
        m_xBlockSize = (size_t)xBlockSize;
        m_yBlockSize = (size_t)yBlockSize;
        m_buf.resize(m_xBlockSize * m_yBlockSize);

        m_xBlockCnt = ((m_xTotalSize - 1) / m_xBlockSize) + 1;
        m_yBlockCnt = ((m_yTotalSize - 1) / m_yBlockSize) + 1;
    }

    /*
      Read the band into the vector.  Reads a block at a time.  Each
      block is either fully populated with data or a partial block.
      Partial blocks appear at the X and Y margins when the total size in
      the doesn't divide evenly by the block size for both the X and Y
      dimensions.

      \param  Data Vector into which the data should be read.  The vector is
        resized as necessary.
    */
    void read(std::vector<T>& data)
    {
        data.resize(m_xTotalSize * m_yTotalSize);

        for (size_t y = 0; y < m_yBlockCnt; ++y)
            for (size_t x = 0; x < m_xBlockCnt; ++x)
                readBlock(x, y, data);
    }

    /*
       Read a block's worth of data.

       Read data into a block-sized buffer.  Then copy data from the
       block buffer into the destination array at the proper location to
       build a complete raster.

       \param x  X coordinate of the block to read.
       \param y  Y coordinate of the block to read.
       \param data  Pointer to the data vector that contains the
          raster information.
     */
    void readBlock(size_t x, size_t y, std::vector<T>& data)
    {
        uint8_t *buf = reinterpret_cast<uint8_t *>(m_buf.data());

        // Block indices are guaranteed not to overflow an int.
        if (m_band->ReadBlock(static_cast<int>(x),
                              static_cast<int>(y), buf) != CPLE_None)
            throw CantReadBlock();

        size_t xWidth = 0;
        if (x == m_xBlockCnt - 1)
            xWidth = m_xTotalSize % m_xBlockSize;
        if (xWidth == 0)
            xWidth = m_xBlockSize;

        size_t yHeight = 0;
        if (y == m_yBlockCnt - 1)
            yHeight = m_yTotalSize % m_yBlockSize;
        if (yHeight == 0)
            yHeight = m_yBlockSize;

        auto bi = m_buf.begin();
        // Go through rows copying data.  Increment the buffer pointer by the
        // width of the row.
        for (size_t row = 0; row < yHeight; ++row)
        {
            size_t wholeRows = m_xTotalSize * ((y * m_yBlockSize) + row);
            size_t partialRows = m_xBlockSize * x;
            auto di = data.begin() + (wholeRows + partialRows);
            std::copy(bi, bi + xWidth, di);

            // Blocks are always full-sized, even if only some of the data
            // is valid, so we use m_xBlockSize instead of xWidth.
            bi += m_xBlockSize;
        }
    }

    /*
      Write linearized data pointed to by \c data into the band.

      \param data  Pointer to beginning of band
    */
    template <typename SOURCE_ITER>
    void write(SOURCE_ITER si, ITER_VAL<SOURCE_ITER> srcNoData)
    {
        for (size_t y = 0; y < m_yBlockCnt; ++y)
            for (size_t x = 0; x < m_xBlockCnt; ++x)
                writeBlock(x, y, si, srcNoData);
    }

    T getNoData() const
    {
        // The destination nodata value was set when the raster was opened.
        // Make sure it's valid for the band type and convert.
        T t;
        if (!Utils::numericCast(m_dstNoData, t))
        {
            throw CantWriteBlock("Invalid nodata value " +
                Utils::toString(m_dstNoData) + " for output data_type '" +
                Utils::typeidName<T>() + "'.");
        }
        return t;
    }

    template <typename SOURCE_ITER>
    void writeBlock(size_t x, size_t y, SOURCE_ITER sourceBegin,
        ITER_VAL<SOURCE_ITER> srcNoData)
    {
        size_t xWidth = 0;
        if (x == m_xBlockCnt - 1)
            xWidth = m_xTotalSize % m_xBlockSize;
        if (xWidth == 0)
            xWidth = m_xBlockSize;

        size_t yHeight = 0;
        if (y == m_yBlockCnt - 1)
            yHeight = m_yTotalSize % m_yBlockSize;
       if (yHeight == 0)
            yHeight = m_yBlockSize;

        T dstNoData = getNoData();
        auto di = m_buf.begin();
        // Go through rows copying data.  Increment the destination iterator
        // by the width of the row.
        for (size_t row = 0; row < yHeight; ++row)
        {
            // Find the offset location in the source container.
            size_t wholeRowElts = m_xTotalSize * ((y * m_yBlockSize) + row);
            size_t partialRowElts = m_xBlockSize * x;

            auto si = sourceBegin + (wholeRowElts + partialRowElts);
            std::transform(si, si + xWidth, di,
                [srcNoData, dstNoData](ITER_VAL<SOURCE_ITER> s){
                    T t;

                    if (srcNoData == s ||
                        (std::isnan(srcNoData) && std::isnan(s)))
                        t = dstNoData;
                    else
                    {
                        if (!Utils::numericCast(s, t))
                        {
                        throw CantWriteBlock("Unable to convert data for "
                            "raster type as requested: " + Utils::toString(s) +
                            " -> " + Utils::typeidName<T>());
                        }
                    }
                    return t;
                });

            // Blocks are always full-sized, even if only some of the data
            // is valid, so we use m_xBlockSize instead of xWidth.
            di += m_xBlockSize;
        }

        //  x and y are guaranteed to fit into an int
        if (m_band->WriteBlock(static_cast<int>(x),
                               static_cast<int>(y), m_buf.data()) != CPLE_None)
            throw CantWriteBlock();
    }

    void statistics(double* minimum, double* maximum,
                    double* mean, double* stddev,
                    int bApprox, int bForce) const
    {
        m_band->GetStatistics(bApprox, bForce, minimum, maximum, mean, stddev);
    }
};


class PDAL_DLL Raster
{
public:
    /**
      Constructor.

      \param filename  Filename of raster file.
      \param drivername  Optional name of driver to use to open raster file.
    */
    Raster(const std::string& filename, const std::string& drivername = "");

    /**
      Constructor.

      \param filename  Filename of raster file.
      \param drivername  Optional name of driver to use to open raster file.
      \param srs  SpatialReference of the raster.
      \param pixelToPos  Transformation matrix to convert raster positions to
        geolocations.
    */
    Raster(const std::string& filename, const std::string& drivername,
        const SpatialReference& srs, const std::array<double, 6> pixelToPos);


    /**
      Destructor.  Closes an open raster.
    */
    ~Raster();

    /**
      Constructor.

      \param ds Pointer to existing GDALDataset
      \param drivername  Optional name of driver to use to open raster file.
      \param srs  SpatialReference of the raster.
      \param pixelToPos  Transformation matrix to convert raster positions to
        geolocations.
    */
    Raster(GDALDataset *ds) : m_ds (ds) {};

    /**
      Return a GDAL MEM driver copy of the raster
    */
    Raster* memoryCopy() const;

    /**
      Open raster file for reading.
    */
    GDALError open();

    /**
      Open a raster for writing.

      \param width  Width of the raster in cells (X direction)
      \param height  Height of the raster in cells (Y direction)
      \param numBands  Number of bands in the raster.
      \param type  Datatype (int, float, etc.) of the raster data.
      \param noData  Value that indiciates no data in the output raster cell.
      \param options  GDAL driver options.
    */
    GDALError open(int width, int height, int numBands, Dimension::Type type,
        double noData, StringList options = StringList());

    /**
      Close the raster and deallocate the underlying dataset.
    */
    void close();

    /**
      Read an entire raster band (layer) into a vector.

      \param band  Vector into which data will be read.  The vector will
        be resized appropriately to hold the data.
      \param nBand  Band number to read.  Band numbers start at 1.
      \return Error code or GDALError::None.
    */
    template<typename T>
    GDALError readBand(std::vector<T>& points, int nBand)
    {
        try
        {
            Band<T>(m_ds, nBand).read(points);
        }
        catch (InvalidBand)
        {
            m_errorMsg = "Unable to get band " + std::to_string(nBand) +
                " from raster '" + m_filename + "'.";
            return GDALError::InvalidBand;
        }
        catch (BadBand)
        {
            m_errorMsg = "Unable to read band/block information from "
                "raster '" + m_filename + "'.";
            return GDALError::BadBand;
        }
        catch (CantReadBlock)
        {
            m_errorMsg = "Unable to read block for for raster '" +
                m_filename + "'.";
            return GDALError::CantReadBlock;
        }
        return GDALError::None;
    }

    /**
      Write an entire raster band (layer) into raster to be written with GDAL.

      \param data  Linearized raster data to be written.
      \param noData  No-data value in the source data.
      \param nBand  Band number to write.
      \param name  Name of the raster band.
    */
    template<typename SOURCE_ITER>
    GDALError writeBand(SOURCE_ITER si, ITER_VAL<SOURCE_ITER> srcNoData,
        int nBand, const std::string& name = "")
    {
        try
        {
            switch(m_bandType)
            {
            case Dimension::Type::Unsigned8:
                Band<uint8_t>(m_ds, nBand, m_dstNoData, name).
                    write(si, srcNoData);
                break;
            case Dimension::Type::Signed8:
                Band<int8_t>(m_ds, nBand, m_dstNoData, name).
                    write(si, srcNoData);
                break;
            case Dimension::Type::Unsigned16:
                Band<uint16_t>(m_ds, nBand, m_dstNoData, name).
                    write(si, srcNoData);
                break;
            case Dimension::Type::Signed16:
                Band<int16_t>(m_ds, nBand, m_dstNoData, name).
                    write(si, srcNoData);
                break;
            case Dimension::Type::Unsigned32:
                Band<uint32_t>(m_ds, nBand, m_dstNoData, name).
                    write(si, srcNoData);
                break;
            case Dimension::Type::Signed32:
                Band<int32_t>(m_ds, nBand, m_dstNoData, name).
                    write(si, srcNoData);
                break;
            case Dimension::Type::Unsigned64:
                Band<uint64_t>(m_ds, nBand, m_dstNoData, name).
                    write(si, srcNoData);
                break;
            case Dimension::Type::Signed64:
                Band<int64_t>(m_ds, nBand, m_dstNoData, name).
                    write(si, srcNoData);
                break;
            case Dimension::Type::Float:
                Band<float>(m_ds, nBand, m_dstNoData, name).
                    write(si, srcNoData);
                break;
            case Dimension::Type::Double:
                Band<double>(m_ds, nBand, m_dstNoData, name).
                    write(si, srcNoData);
                break;
            case Dimension::Type::None:
                throw CantWriteBlock();
            }
        }
        catch (InvalidBand)
        {
            m_errorMsg = "Unable to get band " + std::to_string(nBand) +
                " from raster '" + m_filename + "'.";
            return GDALError::InvalidBand;
        }
        catch (BadBand)
        {
            m_errorMsg = "Unable to read band/block information from "
                "raster '" + m_filename + "'.";
            return GDALError::BadBand;
        }
        catch (CantWriteBlock err)
        {
            m_errorMsg = "Unable to write block for for raster '" +
                m_filename + "'.";
            if (err.what.size())
                m_errorMsg += "\n" + err.what;
            return GDALError::CantWriteBlock;
        }
        return GDALError::None;
    }

    /**
      Read the data for each band at x/y into a vector of doubles.  x and y
      are transformed to the basis of the raster before the data is fetched.

      \param x  X position to read
      \param y  Y position to read
      \param data  Vector in which to store data.
    */
    GDALError read(double x, double y, std::vector<double>& data);

    /**
      Get a vector of dimensions that map to the bands of a raster.
    */
    std::vector<pdal::Dimension::Type> getPDALDimensionTypes() const
       { return m_types; }

    /**
      Convert an X/Y raster position into geo-located position using the
      raster's transformation matrix.

      \param column  raster column whose position should be calculated
      \param row  raster row whose position should be calculated
      \param[out]  Array containing the geo-located position of the pixel.
    */
    void pixelToCoord(int column, int row, std::array<double, 2>& output) const;

    /**
      Get the spatial reference associated with the raster.

      \return  The associated spatial reference.
    */
    SpatialReference getSpatialRef() const;

    /**
      Get the most recent error message.
    */
    std::string errorMsg() const
        { return m_errorMsg; }

    /**
      Get the number of bands in the raster.

      \return  The number of bands in the raster.
    */
    int bandCount() const
        { return m_numBands; }

    /**
      Get the width of the raster (X direction)
    */
    int width() const
        { return m_width; }

    /**
      Get the height of the raster (Y direction)
    */
    int height() const
        { return m_height; }

    std::string const& filename() { return m_filename; }

    void statistics(int nBand, double* minimum, double* maximum, double* mean,
        double* stddev, int bApprox = TRUE, int bForce = TRUE) const
    {
        Band<double>(m_ds, nBand).statistics(minimum, maximum, mean, stddev,
            bApprox, bForce);
    }

    BOX2D bounds() const
    {
        std::array<double, 2> coords;

        pixelToCoord(height(), width(), coords);
        double maxx = coords[0];
        double maxy = coords[1];

        pixelToCoord(0, 0, coords);
        double minx = coords[0];
        double miny = coords[1];
        return BOX2D(minx, miny, maxx, maxy);
    }

    BOX3D bounds(int nBand) const
    {
        BOX2D box2 = bounds();

        double minimum; double maximum;
        double mean; double stddev;
        statistics(nBand, &minimum, &maximum, &mean, &stddev);

        return BOX3D(box2.minx, box2.miny, minimum,
                     box2.maxx, box2.maxy, maximum);
    }

private:
    std::string m_filename;

    int m_width;
    int m_height;
    int m_numBands;
    std::string m_drivername;
    std::array<double, 6> m_forwardTransform;
    std::array<double, 6> m_inverseTransform;
    SpatialReference m_srs;
    GDALDataset *m_ds;
    Dimension::Type m_bandType;
    double m_dstNoData;

    GDALError wake();

    std::string m_errorMsg;
    mutable std::vector<pdal::Dimension::Type> m_types;
    std::vector<std::array<double, 2>> m_block_sizes;

    GDALError validateType(Dimension::Type& type, GDALDriver *driver);
    bool getPixelAndLinePosition(double x, double y,
        int32_t& pixel, int32_t& line);
    GDALError computePDALDimensionTypes();
};


} // namespace gdal

namespace gdal
{
OGRGeometry *createFromWkt(const char *s);
OGRGeometry *createFromGeoJson(const char *s);

// New signatures to support extraction of SRS from the end of geometry
// specifications..
OGRGeometry *createFromWkt(const std::string& s, std::string& srs);
OGRGeometry *createFromGeoJson(const std::string& s, std::string& srs);

inline OGRGeometry *fromHandle(OGRGeometryH geom)
{ return reinterpret_cast<OGRGeometry *>(geom); }

inline OGRGeometryH toHandle(OGRGeometry *h)
{ return reinterpret_cast<OGRGeometryH>(h); }

} // namespace gdal

} // namespace pdal
