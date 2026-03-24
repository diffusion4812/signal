#ifndef LINE_PLOT_H
#define LINE_PLOT_H

#include "signal_ui/text.h"
#include <wx/wx.h>
#include <wx/glcanvas.h>
#include <wx/settings.h>
#include <vector>
#include <memory>
#include <string>
#include <map>
#include <cstdint>

// Forward declarations
typedef struct FT_LibraryRec_*  FT_Library;
typedef struct FT_FaceRec_*     FT_Face;

// ---------------------------------------------------------------------------
// RGB colour helper
// ---------------------------------------------------------------------------
struct RGB {
    float r, g, b;
};

// ---------------------------------------------------------------------------
// Time axis support
// ---------------------------------------------------------------------------
enum class TimeResolution {
    Nanoseconds,
    Microseconds,
    Milliseconds,
    Seconds,
    Minutes,
    Hours,
    Days
};

struct TimeAxisConfig {
    bool enabled;
    
    // Format selection based on zoom level
    TimeResolution detectedResolution;
    std::function<wxString(int64_t, TimeResolution)> formatFunc;
    
    // Cache for performance
    mutable uint64_t cachedTimestamp;
    mutable wxString cachedResult;
    mutable TimeResolution cachedResolution;
    
    TimeAxisConfig();
    
    // Determine appropriate resolution based on time range
    static TimeResolution DetermineResolution(uint64_t range_ns);
};

// ---------------------------------------------------------------------------
// AxisPosition – which edge of the canvas an axis is pinned to.
// ---------------------------------------------------------------------------
enum class AxisPosition {
    AtZero,
    Left,
    Right,
    Top,
    Bottom
};

// ---------------------------------------------------------------------------
// LegendPosition – which corner of the plot area the legend is anchored to.
// ---------------------------------------------------------------------------
enum class LegendPosition {
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight
};

// ---------------------------------------------------------------------------
// YAxisConfig – configuration for a single Y-axis
// ---------------------------------------------------------------------------
struct YAxisConfig {
    double min, max;
    RGB colour;
    wxString label;
    AxisPosition position;  // Left or Right
    int stackOrder;         // 0 = outermost, 1 = next inward, etc.
    bool enabled;
    bool autoscale;
    
    YAxisConfig()
        : min(-1.0), max(1.0)
        , colour{1.0f, 1.0f, 1.0f}
        , label("")
        , position(AxisPosition::Left)
        , stackOrder(0)
        , enabled(true)
        , autoscale(false)
    {}
};

// ---------------------------------------------------------------------------
// PlotSeries – one named, coloured data series.
// ---------------------------------------------------------------------------
struct PlotSeries {
    std::vector<uint64_t> xs;
    std::vector<double> ys;
    wxString            name;
    RGB                 colour = {0.0f, 0.0f, 0.0f};
    int                 yAxisIndex = 0;  // Which Y-axis this series uses

    // --- Fill/shading support -----------------------------------------------
    bool   fillEnabled;
    double fillBaseY;       // Fill from curve to this Y value (in data coords)
    float  fillAlpha;       // Transparency for fill (0.0 = transparent, 1.0 = opaque)

    // --- Boolean-series support ---------------------------------------------
    // When isBoolean is true the series is rendered as a row of rectangles
    // rather than a polyline.
    bool   isBoolean;
    double boolBaseY;       // bottom edge of the boolean band (data coords)
    double boolBarHeight;   // height of the boolean band  (data coords)
    
    PlotSeries()
        : fillEnabled(false)
        , fillBaseY(0.0)
        , fillAlpha(0.3f)
        , isBoolean(false)
        , boolBaseY(0.0)
        , boolBarHeight(1.0)
    {}
};

class LinePlot : public wxGLCanvas {
public:
    // -----------------------------------------------------------------------
    // Construction
    // -----------------------------------------------------------------------
    LinePlot(wxWindow* parent);
    ~LinePlot();

    // -----------------------------------------------------------------------
    // Data – multi-series
    // -----------------------------------------------------------------------
    void SetSeries(const std::vector<PlotSeries>& series);
    void AddSeries(const std::vector<uint64_t>& xs, const std::vector<double>& ys,
                   const wxString& name, RGB colour, int yAxisIndex = 0);
    void ClearSeries();

    // -----------------------------------------------------------------------
    // X-Axes management
    // -----------------------------------------------------------------------
    void ResetTimeBase(); // Set startTime_ to current system clock
    void SetTimeBase(uint64_t start_ns) { startTime_ = start_ns; buffersDirty_ = true; }
    uint64_t GetTimeBase() const { return startTime_; }
    void SetXAxisLimits(uint64_t xMin, uint64_t xMax);
    void SetAutoscaleX(bool on);
    void SetXAxisPosition(AxisPosition pos);

    // -----------------------------------------------------------------------
    // Y-Axes management
    // -----------------------------------------------------------------------
    int AddYAxis(const YAxisConfig& config);
    int AddYAxis();  // Add with defaults
    void SetYAxisConfig(int axisIndex, const YAxisConfig& config);
    YAxisConfig GetYAxisConfig(int axisIndex) const;
    int GetYAxisCount() const { return static_cast<int>(yAxes_.size()); }
    void RemoveYAxis(int axisIndex);

    // -----------------------------------------------------------------------
    // Axes appearance
    // -----------------------------------------------------------------------
    void SetAxesEnabled(bool enabled);

    // -----------------------------------------------------------------------
    // Grid
    // -----------------------------------------------------------------------
    void SetGridEnabled(bool enabled);
    void SetGridSpacingX(double spacing);
    void SetGridSpacingY(double spacing);
    void SetGridColour(float r, float g, float b);

    // -----------------------------------------------------------------------
    // Ticks
    // -----------------------------------------------------------------------
    void SetTicksEnabled(bool enabled);
    void SetTickSpacingX(double spacing);
    void SetTickSpacingY(double spacing);
    void SetTickLength(int pixels);

    // -----------------------------------------------------------------------
    // Labels
    // -----------------------------------------------------------------------
    void SetLabelsEnabled(bool enabled);
    void SetLabelPrecision(int decimals);

    // -----------------------------------------------------------------------
    // Title
    // -----------------------------------------------------------------------
    void SetTitle(const wxString& title);

    // -----------------------------------------------------------------------
    // Legend
    // -----------------------------------------------------------------------
    void SetLegendEnabled(bool enabled);
    void SetLegendPosition(LegendPosition pos);

    // -----------------------------------------------------------------------
    // Time axis (X-axis timestamps in nanoseconds)
    // -----------------------------------------------------------------------
    
    /// Enable timestamp mode for X-axis
    /// All timestamps are in nanoseconds since Unix epoch
    void SetTimeAxisEnabled(bool enabled);
    
    /// Set custom time formatting function
    /// Function receives int64_t timestamp (nanoseconds) and TimeResolution
    void SetTimeFormatFunction(std::function<wxString(int64_t, TimeResolution)> func);
    
    /// Get current time axis configuration
    TimeAxisConfig GetTimeAxisConfig() const { return timeAxisConfig_; }

protected:
    void OnPaint(wxPaintEvent& event);
    void OnResize(wxSizeEvent& event);
    void OnMouseMotion(wxMouseEvent& event);
    void OnSysColourChanged(wxSysColourChangedEvent& event);

private:
    // -----------------------------------------------------------------------
    // Colours
    // -----------------------------------------------------------------------

    RGB bg_colour_;
    const RGB bg_colour_palette_[2] = {
        {0.1f, 0.1f, 0.1f},    // Dark
        {0.93f, 0.93f, 0.93f}, // Light
    };

    RGB grid_colour_;
    const RGB grid_colour_palette_[2] = {
        {0.8f, 0.8f, 0.8f},    // Dark
        {0.23f, 0.23f, 0.23f}, // Light
    };

    RGB text_colour_;

    const RGB plot_colour_palette_[5] = {
        {0.0f, 0.45f, 0.74f},  // Blue
        {0.85f, 0.33f, 0.10f}, // Orange
        {0.93f, 0.69f, 0.13f}, // Yellow
        {0.49f, 0.18f, 0.56f}, // Purple
        {0.47f, 0.67f, 0.19f}  // Green
    };

    // --- GL context --------------------------------------------------------
    wxGLContext* context_;
    bool glInitialized_;

    // --- data --------------------------------------------------------------
    std::vector<PlotSeries> series_;

    // --- view (X-axis, double precision for data coordinates) --------------
    uint64_t startTime_;
    uint64_t xMin_, xMax_;
    bool autoscaleX_;
    AxisPosition xAxisPos_;

    // --- Y-axes (multiple, independent) ------------------------------------
    std::vector<YAxisConfig> yAxes_;

    // --- axes --------------------------------------------------------------
    bool axesEnabled_;

    // --- grid (double precision for spacing) -------------------------------
    bool   gridEnabled_;
    double gridSpacingX_, gridSpacingY_;
    float  gridColourR_, gridColourG_, gridColourB_;

    // --- ticks (double precision for spacing) ------------------------------
    bool   ticksEnabled_;
    double tickSpacingX_, tickSpacingY_;
    int    tickLengthPx_;

    // --- labels ------------------------------------------------------------
    bool labelsEnabled_;
    int  labelPrecision_;

    // --- title -------------------------------------------------------------
    wxString title_;

    // --- legend ------------------------------------------------------------
    bool           legendEnabled_;
    LegendPosition legendPos_;
    float legend_bg_alpha_;
    RGB legend_bg_colour_;
    RGB legend_border_colour_;

    // --- crosshair ---------------------------------------------------------
    int mouseX_;
    int mouseY_;

    // --- margins -----------------------------------------------------------
    int marginLeft_, marginRight_, marginTop_, marginBottom_;

    // --- time axis ---------------------------------------------------------
    TimeAxisConfig timeAxisConfig_;

    // -----------------------------------------------------------------------
    // OpenGL 3.3+ resources
    // -----------------------------------------------------------------------
    
    // Shader programs
    GLuint lineShader_;
    GLuint fillShader_;
    GLuint gridShader_;
    GLuint textShader_;
    
    // VBOs/VAOs for different elements
    struct SeriesBuffers {
        unsigned int vao;
        unsigned int vbo;
        size_t vertexCount;
        int yAxisIndex;
    };
    std::vector<SeriesBuffers> seriesBuffers_;
    
    unsigned int gridVAO_, gridVBO_;
    unsigned int axisVAO_, axisVBO_;
    unsigned int rectVAO_, rectVBO_;
    
    bool buffersDirty_;

    // -----------------------------------------------------------------------
    // FreeType
    // -----------------------------------------------------------------------

    FT_Library ftLibrary_;
    signal_ui::FontAtlas labelAtlas_;
    signal_ui::FontAtlas titleAtlas_;
    float titleFontSize_ = 24.0f;   // desired pixel height for title text
    float labelFontSize_ = 12.0f;   // desired pixel height for axis labels

    // -----------------------------------------------------------------------
    // OpenGL initialization
    // -----------------------------------------------------------------------
    void InitializeGL();
    unsigned int CompileShader(const char* vertexSrc, const char* fragmentSrc);
    void CheckShaderCompileErrors(unsigned int shader, const std::string& type);
    void CheckProgramLinkErrors(unsigned int program);

    // -----------------------------------------------------------------------
    // Layout & projection
    // -----------------------------------------------------------------------
    template <typename T>
    static T NiceStep(T lo, T hi);
    uint64_t EffectiveTickSpacingX() const;
    float EffectiveTickSpacingY(int yAxisIndex) const;
    float XAxisY() const;
    
    /// Get the position of a Y-axis in data coordinates
    /// For Left axes: returns xMin_ (left edge)
    /// For Right axes: returns xMax_ (right edge)
    float YAxisX(int yAxisIndex) const;
    
    /// Get the screen X position for a Y-axis (accounting for stacking)
    int YAxisScreenX(int yAxisIndex) const;

    /// Recompute axis limits from data when autoscale is enabled
    void ApplyAutoscale();

    void ComputeMargins();

    void UpdateColoursFromSystem();

    struct FloatRect {
        float x, y, width, height;
        float GetLeft() const { return x; }
        float GetRight() const { return x + width; }
        float GetTop() const { return y; }
        float GetBottom() const { return y + height; }
        float GetWidth() const { return width; }
        float GetHeight() const { return height; }
    };
    FloatRect PlotRect() const;
    
    /// Convert data coordinates to screen coordinates
    /// yAxisIndex specifies which Y-axis to use for vertical scaling
    void DataToScreen(uint64_t dx, double dy, int yAxisIndex, float& sx, float& sy) const;

    // -----------------------------------------------------------------------
    // Buffer management
    // -----------------------------------------------------------------------
    void UpdateBuffers();
    void UpdateSeriesBuffers();
    void UpdateGridBuffers();
    void UpdateAxisBuffers();

    // -----------------------------------------------------------------------
    // Rendering – all use modern OpenGL
    // -----------------------------------------------------------------------
    void RenderFilledRect(float x, float y, float w, float h,
                          const glm::vec4& colour,
                          const glm::mat4& projection);
    void RenderRectOutline(float x, float y, float w, float h,
                           const glm::vec3& colour,
                           const glm::mat4& projection);
    void RenderGrid();
    void RenderAxes();
    void RenderSeries();
    void RenderOverlay();   // title + tick labels
    void RenderLegend(const glm::mat4& overlay, int w, int h);
    void RenderCrosshair();

    wxDECLARE_EVENT_TABLE();
};

#endif // LINE_PLOT_H