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

struct ChunkView {
    const int64_t*  xs  = nullptr;
    const float*    ys  = nullptr;
    int64_t        len = 0;

    // Bridge point from the tail of the previous chunk
    // Valid only if hasBridge == true
    bool    hasBridge   = false;
    int64_t bridgeTime  = 0;
    float  bridgeValue = 0.0;
};

struct PlotSeries {
    std::vector<ChunkView> chunks;
    wxString       name;
    RGB            colour = {0.0f, 0.0f, 0.0f};
    int            yAxisIndex = 0;  // Which Y-axis this series uses

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
    void ClearSeries();
    void SyncYAxes();

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
    struct ShaderUniforms {
        GLint color = -1;
        GLint xMin  = -1;
        GLint xMax  = -1;
        GLint yMin  = -1;
        GLint yMax  = -1;
    };
    ShaderUniforms uniforms_;
    GLuint fillShader_;
    GLuint gridShader_;
    GLuint textShader_;
    
    // VBOs/VAOs for different elements
    struct SeriesBuffers {
        GLuint  vao          = 0;
        GLuint  vbo_xs       = 0;
        GLuint  vbo_ys       = 0;
        GLsizei vertexCount  = 0;
        GLsizei allocatedCap = 0;

        int  yAxisIndex = 0;
        bool isBoolean  = false;
        RGB  colour;

        float xMin = 0.f, xMax = 1.f;
        float yMin = 0.f, yMax = 1.f;

        // Timestamp scratch — CPU conversion unavoidable for int64 → float
        std::vector<float> tScratch;

        bool valid() const { return vao != 0; }

        void Allocate(GLsizei capacity) {
            glGenVertexArrays(1, &vao);
            glGenBuffers(1, &vbo_xs);
            glGenBuffers(1, &vbo_ys);

            glBindBuffer(GL_ARRAY_BUFFER, vbo_xs);
            glBufferData(GL_ARRAY_BUFFER, capacity * sizeof(float),
                        nullptr, GL_STREAM_DRAW);

            glBindBuffer(GL_ARRAY_BUFFER, vbo_ys);
            glBufferData(GL_ARRAY_BUFFER, capacity * sizeof(float),
                        nullptr, GL_STREAM_DRAW);

            glBindVertexArray(vao);

            glBindBuffer(GL_ARRAY_BUFFER, vbo_xs);
            glVertexAttribPointer(0, 1, GL_FLOAT, GL_FALSE, 0, nullptr);
            glEnableVertexAttribArray(0);

            glBindBuffer(GL_ARRAY_BUFFER, vbo_ys);
            glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, 0, nullptr);
            glEnableVertexAttribArray(1);

            glBindVertexArray(0);

            allocatedCap = capacity;
        }

        void Free() {
            if (vao)    { glDeleteVertexArrays(1, &vao);    vao    = 0; }
            if (vbo_xs) { glDeleteBuffers(1, &vbo_xs);      vbo_xs = 0; }
            if (vbo_ys) { glDeleteBuffers(1, &vbo_ys);      vbo_ys = 0; }
            allocatedCap = 0;
            vertexCount  = 0;
            tScratch.clear();
        }

        // Ensures GPU buffers can hold at least `count` vertices
        void EnsureCapacity(GLsizei count) {
            if (count <= allocatedCap) return;

            GLsizei newCap = count * 2;

            glBindBuffer(GL_ARRAY_BUFFER, vbo_xs);
            glBufferData(GL_ARRAY_BUFFER, newCap * sizeof(float),
                        nullptr, GL_STREAM_DRAW);

            glBindBuffer(GL_ARRAY_BUFFER, vbo_ys);
            glBufferData(GL_ARRAY_BUFFER, newCap * sizeof(float),
                        nullptr, GL_STREAM_DRAW);

            allocatedCap = newCap;
        }

        // Upload timestamp scratch (full buffer — always CPU-side float array)
        void UploadXs(GLsizei count) {
            glBindBuffer(GL_ARRAY_BUFFER, vbo_xs);
            glBufferData(GL_ARRAY_BUFFER, allocatedCap * sizeof(float),
                        nullptr, GL_STREAM_DRAW);
            glBufferSubData(GL_ARRAY_BUFFER, 0,
                            count * sizeof(float), tScratch.data());
        }

        // Upload value channel — split into optional bridge + direct Arrow buffer
        void UploadYs(GLsizei count,
                    const float* arrowBuffer,
                    GLsizei      arrowLen,
                    const float* bridgeValue = nullptr)
        {
            glBindBuffer(GL_ARRAY_BUFFER, vbo_ys);
            glBufferData(GL_ARRAY_BUFFER, allocatedCap * sizeof(float),
                        nullptr, GL_STREAM_DRAW);

            if (bridgeValue) {
                // Prepend bridge point — one float
                glBufferSubData(GL_ARRAY_BUFFER,
                    0, sizeof(float), bridgeValue);

                // Upload Arrow buffer directly — zero copy
                glBufferSubData(GL_ARRAY_BUFFER,
                    sizeof(float), arrowLen * sizeof(float), arrowBuffer);
            } else {
                // Direct upload — Arrow buffer untouched on CPU
                glBufferSubData(GL_ARRAY_BUFFER,
                    0, count * sizeof(float), arrowBuffer);
            }

            vertexCount = count;
        }
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

    std::pair<float, float> ComputeYRange(const std::vector<ChunkView>& chunks, float paddingFraction);

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