#include <GL/glew.h>
#include <GL/gl.h>
#include <cmath>
#include <sstream>
#include <set>
#include <iomanip>
#include <chrono>
#include <type_traits>
#include <algorithm>
#include <ctime>
#include <ft2build.h>
#include FT_FREETYPE_H

#include "line_plot.h"

// ===========================================================================
// TimeAxisConfig implementation
// ===========================================================================

TimeAxisConfig::TimeAxisConfig()
    : enabled(true)
    , detectedResolution(TimeResolution::Seconds)
    , cachedTimestamp(-1)
    , cachedResolution(TimeResolution::Seconds)
{
    // Default formatter - automatically selects format based on resolution
    formatFunc = [](uint64_t timestamp_ns, TimeResolution resolution) -> wxString {
        // Convert nanoseconds to seconds
        time_t seconds = static_cast<time_t>(timestamp_ns / 1000000000LL);
        uint64_t nanoseconds = timestamp_ns % 1000000000LL;
        
        std::tm* timeinfo = std::localtime(&seconds);
        
        switch (resolution) {
            case TimeResolution::Nanoseconds: {
                // HH:MM:SS.nnnnnnnnn\n YYYY-MM-DD
                int ns = nanoseconds % 1000;
                int us = (nanoseconds / 1000) % 1000;
                int ms = (nanoseconds / 1000000) % 1000;
                
                return wxString::Format("%02d:%02d:%02d.%03d%03d%03d\n%04d-%02d-%02d",
                    timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec,
                    ms, us, ns,
                    timeinfo->tm_year + 1900, timeinfo->tm_mon + 1, timeinfo->tm_mday);
            }
            
            case TimeResolution::Microseconds: {
                // HH:MM:SS.uuuuuu\nYYYY-MM-DD
                int microseconds = (nanoseconds / 1000);
                
                return wxString::Format("%02d:%02d:%02d.%06d\n%04d-%02d-%02d",
                    timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec,
                    static_cast<int>(microseconds),
                    timeinfo->tm_year + 1900, timeinfo->tm_mon + 1, timeinfo->tm_mday);
            }
            
            case TimeResolution::Milliseconds: {
                // HH:MM:SS.mmm\nYYYY-MM-DD
                int milliseconds = (nanoseconds / 1000000) % 1000;
                
                return wxString::Format("%02d:%02d:%02d.%03d\n%04d-%02d-%02d",
                    timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec,
                    milliseconds,
                    timeinfo->tm_year + 1900, timeinfo->tm_mon + 1, timeinfo->tm_mday);
            }
            
            case TimeResolution::Seconds: {
                // HH:MM:SS\nYYYY-MM-DD
                return wxString::Format("%02d:%02d:%02d\n%04d-%02d-%02d",
                    timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec,
                    timeinfo->tm_year + 1900, timeinfo->tm_mon + 1, timeinfo->tm_mday);
            }
            
            case TimeResolution::Minutes: {
                // HH:MM\nYYYY-MM-DD
                return wxString::Format("%02d:%02d\n%04d-%02d-%02d",
                    timeinfo->tm_hour, timeinfo->tm_min,
                    timeinfo->tm_year + 1900, timeinfo->tm_mon + 1, timeinfo->tm_mday);
            }
            
            case TimeResolution::Hours: {
                // HH:00\nDDD MMM DD
                static const char* months[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                               "Jul","Aug","Sep","Oct","Nov","Dec"};
                static const char* days[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
                
                return wxString::Format("%02d:00\n%s %s %d",
                    timeinfo->tm_hour,
                    days[timeinfo->tm_wday],
                    months[timeinfo->tm_mon],
                    timeinfo->tm_mday);
            }
            
            case TimeResolution::Days: {
                // DDD MMM DD\nYYYY
                static const char* months[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                               "Jul","Aug","Sep","Oct","Nov","Dec"};
                static const char* days[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
                
                return wxString::Format("%s %s %d\n%04d",
                    days[timeinfo->tm_wday],
                    months[timeinfo->tm_mon],
                    timeinfo->tm_mday,
                    timeinfo->tm_year + 1900);
            }
        }
        
        return "";
    };
}

TimeResolution TimeAxisConfig::DetermineResolution(uint64_t range_ns) {
    // Convert to more useful units for comparison
    double range_us = range_ns / 1000.0;
    double range_ms = range_ns / 1000000.0;
    double range_s = range_ns / 1000000000.0;
    double range_min = range_s / 60.0;
    double range_hr = range_min / 60.0;
    double range_day = range_hr / 24.0;
    
    // Select resolution based on visible range
    if (range_ns < 10000.0) {                    // < 10 microseconds
        return TimeResolution::Nanoseconds;
    }
    else if (range_us < 10000.0) {              // < 10 milliseconds
        return TimeResolution::Microseconds;
    }
    else if (range_ms < 60000.0) {              // < 60 seconds
        return TimeResolution::Milliseconds;
    }
    else if (range_s < 600.0) {                 // < 10 minutes
        return TimeResolution::Seconds;
    }
    else if (range_min < 180.0) {               // < 3 hours
        return TimeResolution::Minutes;
    }
    else if (range_hr < 72.0) {                 // < 3 days
        return TimeResolution::Hours;
    }
    else {
        return TimeResolution::Days;
    }
}

// ---------------------------------------------------------------------------
// OpenGL 3.3+ Core Profile attributes
// ---------------------------------------------------------------------------
static const int glAttribs[] = {
    WX_GL_RGBA,
    WX_GL_DOUBLEBUFFER, 1,
    WX_GL_DEPTH_SIZE, 0,
    WX_GL_CORE_PROFILE,          // Request core profile
    WX_GL_MAJOR_VERSION, 3,
    WX_GL_MINOR_VERSION, 3,
    WX_GL_SAMPLE_BUFFERS, 1,     // MSAA
    WX_GL_SAMPLES, 4,            // MSAA
    0
};

// ---------------------------------------------------------------------------
// Shader source code
// ---------------------------------------------------------------------------

// Simple vertex shader for lines and fills (2D)
static const char* simpleVertexShader = R"(
#version 330 core
layout (location = 0) in vec2 aPos;

uniform mat4 u_proj;

void main() {
    gl_Position = u_proj * vec4(aPos, 0.0, 1.0);
}
)";

static const char* seriesShader = R"(
// Vertex shader
layout(location = 0) in float aX;   // data-space x (relative float32)
layout(location = 1) in float aY;   // data-space y (float32)

uniform mat4 uProjection;           // same screen-space ortho matrix as axis shader

uniform float uXMin;
uniform float uXMax;
uniform float uYMin;
uniform float uYMax;

uniform float uScrLeft;
uniform float uScrRight;
uniform float uScrTop;
uniform float uScrBottom;

void main()
{
    // Normalise data to [0, 1]
    float nx = (aX - uXMin) / (uXMax - uXMin);
    float ny = (aY - uYMin) / (uYMax - uYMin);

    // Map to screen space — same coordinate system as axis vertices
    float sx = uScrLeft   + nx * (uScrRight  - uScrLeft);
    float sy = uScrBottom + ny * (uScrTop    - uScrBottom);

    // Projection handles screen → NDC
    gl_Position = uProjection * vec4(sx, sy, 0.0, 1.0);
}
)";

// Fragment shader for solid colour
static const char* solidColourFragmentShader = R"(
#version 330 core
out vec4 FragColor;

uniform vec4 u_color;

void main() {
    FragColor = u_color;
}
)";

// ---------------------------------------------------------------------------
// Event table
// ---------------------------------------------------------------------------
wxBEGIN_EVENT_TABLE(LinePlot, wxGLCanvas)
    EVT_PAINT(LinePlot::OnPaint)
    EVT_SIZE(LinePlot::OnResize)
    EVT_MOTION(LinePlot::OnMouseMotion)
    EVT_SYS_COLOUR_CHANGED(LinePlot::OnSysColourChanged)
    EVT_MOUSEWHEEL(LinePlot::OnMouseWheel)
wxEND_EVENT_TABLE()

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------
LinePlot::LinePlot(wxWindow* parent)
    : wxGLCanvas(parent, wxID_ANY, glAttribs),
      context_(new wxGLContext(this)),
      xMin_(0.0), xMax_(1.0),
      autoscaleX_(false),
      xAxisPos_(AxisPosition::Bottom),
      gridEnabled_(true),
      gridSpacingX_(0.0), gridSpacingY_(0.0),
      ticksEnabled_(true),
      tickSpacingX_(0.0), tickSpacingY_(0.0),
      tickLengthPx_(6),
      labelsEnabled_(true),
      labelPrecision_(2),
      legendEnabled_(true),
      legendPos_(LegendPosition::TopRight),
      marginLeft_(0), marginRight_(0), marginTop_(0), marginBottom_(0),
      glInitialized_(false),
      seriesShader_(0), lineShader_(0), fillShader_(0), gridShader_(0),
      gridVAO_(0), gridVBO_(0),
      axisVAO_(0), axisVBO_(0),
      rectVAO_(0), rectVBO_(0),
      buffersDirty_(true)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);

    UpdateColoursFromSystem(); // update colours

    ResetTimeBase();

    legend_bg_alpha_      = 0.75f;
    legend_bg_colour_     = { 0.15f, 0.15f, 0.15f };
    legend_border_colour_ = { 0.45f, 0.45f, 0.45f };

    // Create default Y-axis
    AddYAxis();
}

LinePlot::~LinePlot()
{
    if (glInitialized_)
    {
        SetCurrent(*context_);
        
        // Clean up FreeType
        signal_ui::CleanupFreeTypeLib(ftLibrary_);
        
        // Delete shaders
        if (seriesShader_) glDeleteProgram(seriesShader_);
        if (lineShader_) glDeleteProgram(lineShader_);
        if (fillShader_) glDeleteProgram(fillShader_);
        if (gridShader_) glDeleteProgram(gridShader_);
        
        // Delete buffers
        for (auto& sb : seriesBuffers_) {
            glDeleteVertexArrays(1, &sb.vao);
            glDeleteBuffers(1, &sb.vbo_xs);
            glDeleteBuffers(1, &sb.vbo_ys);
        }
        
        if (gridVAO_) glDeleteVertexArrays(1, &gridVAO_);
        if (gridVBO_) glDeleteBuffers(1, &gridVBO_);
        if (axisVAO_) glDeleteVertexArrays(1, &axisVAO_);
        if (axisVBO_) glDeleteBuffers(1, &axisVBO_);
    }
    
    delete context_;
}

// ---------------------------------------------------------------------------
// Public setters – data
// ---------------------------------------------------------------------------
void LinePlot::SetSeries(const std::vector<PlotSeries>& series)
{
    series_ = series;
    int i = 0;
    for (auto& s : series_) {
        if (s.colour.r == 0.0f && s.colour.g == 0.0f && s.colour.b == 0.0f) {
            s.colour = plot_colour_palette_[i % 5];
            i++;
        }
    }
    buffersDirty_ = true;
    Refresh();
}

void LinePlot::ClearSeries()
{
    series_.clear();
    buffersDirty_ = true;
    Refresh();
}

void LinePlot::SyncYAxes()
{
    std::set<int> usedIndices;
    for (const auto& s : series_)
        usedIndices.insert(s.yAxisIndex);

    const int required = usedIndices.empty()
        ? 0
        : *usedIndices.rbegin() + 1;

    const int current = static_cast<int>(yAxes_.size());

    if (required == current) return;

    if (required > current) {
        for (int i = current; i < required; ++i) {
            YAxisConfig axis;
            axis.min       =  0.0;
            axis.max       =  1.0;
            axis.autoscale = true;
            axis.colour    = plot_colour_palette_[i % 5];
            yAxes_.push_back(axis);
        }
    } else {
        yAxes_.resize(required);
    }
}

// ---------------------------------------------------------------------------
// X-Axes management
// ---------------------------------------------------------------------------
void LinePlot::ResetTimeBase() {
    auto now = std::chrono::system_clock::now().time_since_epoch();
    startTime_ = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    buffersDirty_ = true;
}

void LinePlot::SetXAxisLimits(uint64_t xMin, uint64_t xMax)
{
    xMin_ = xMin; xMax_ = xMax;
    buffersDirty_ = true;
}
void LinePlot::SetAutoscaleX(bool on) { autoscaleX_ = on; }
void LinePlot::SetXAxisPosition(AxisPosition pos) { xAxisPos_ = pos; }
void LinePlot::SetAxesEnabled(bool enabled) { axesEnabled_ = enabled; }

// ---------------------------------------------------------------------------
// Y-Axes management
// ---------------------------------------------------------------------------
int LinePlot::AddYAxis(const YAxisConfig& config)
{
    yAxes_.push_back(config);
    buffersDirty_ = true;
    Refresh();
    return static_cast<int>(yAxes_.size()) - 1;
}

int LinePlot::AddYAxis()
{
    YAxisConfig config;
    config.min = -1.0;
    config.max = 1.0;
    config.colour = {1.0f, 1.0f, 1.0f};
    config.position = AxisPosition::Left;
    config.stackOrder = static_cast<int>(yAxes_.size());
    config.enabled = true;
    config.autoscale = true;
    return AddYAxis(config);
}

void LinePlot::SetYAxisConfig(int axisIndex, const YAxisConfig& config)
{
    if (axisIndex >= 0 && axisIndex < static_cast<int>(yAxes_.size())) {
        yAxes_[axisIndex] = config;
        buffersDirty_ = true;
        Refresh();
    }
}

YAxisConfig LinePlot::GetYAxisConfig(int axisIndex) const
{
    if (axisIndex >= 0 && axisIndex < static_cast<int>(yAxes_.size())) {
        return yAxes_[axisIndex];
    }
    return YAxisConfig();
}

void LinePlot::RemoveYAxis(int axisIndex)
{
    if (axisIndex > 0 && axisIndex < static_cast<int>(yAxes_.size())) {
        yAxes_.erase(yAxes_.begin() + axisIndex);
        // Update series that reference this axis
        for (auto& s : series_) {
            if (s.yAxisIndex == axisIndex) {
                s.yAxisIndex = 0;  // Reassign to first axis
            } else if (s.yAxisIndex > axisIndex) {
                s.yAxisIndex--;  // Shift down
            }
        }
        buffersDirty_ = true;
        Refresh();
    }
}

// ---------------------------------------------------------------------------
// Public setters – grid
// ---------------------------------------------------------------------------
void LinePlot::SetGridEnabled(bool enabled)   { gridEnabled_ = enabled; buffersDirty_ = true; }
void LinePlot::SetGridSpacingX(double spacing) { gridSpacingX_ = spacing; buffersDirty_ = true; }
void LinePlot::SetGridSpacingY(double spacing) { gridSpacingY_ = spacing; buffersDirty_ = true; }

// ---------------------------------------------------------------------------
// Public setters – ticks & labels
// ---------------------------------------------------------------------------
void LinePlot::SetTicksEnabled(bool enabled)   { ticksEnabled_ = enabled; }
void LinePlot::SetTickSpacingX(double spacing)  { tickSpacingX_ = spacing; }
void LinePlot::SetTickSpacingY(double spacing)  { tickSpacingY_ = spacing; }
void LinePlot::SetTickLength(int pixels)       { tickLengthPx_ = pixels; }
void LinePlot::SetLabelsEnabled(bool enabled)  { labelsEnabled_ = enabled; }
void LinePlot::SetLabelPrecision(int decimals) { labelPrecision_ = decimals; }

// ---------------------------------------------------------------------------
// Public setters – title & legend
// ---------------------------------------------------------------------------
void LinePlot::SetTitle(const wxString& title)
{
    title_ = title;
}
void LinePlot::SetLegendEnabled(bool enabled)        { legendEnabled_ = enabled; }
void LinePlot::SetLegendPosition(LegendPosition pos) { legendPos_ = pos; }

// ---------------------------------------------------------------------------
// Time axis configuration
// ---------------------------------------------------------------------------

void LinePlot::SetTimeAxisEnabled(bool enabled)
{
    timeAxisConfig_.enabled = enabled;
    timeAxisConfig_.cachedTimestamp = -1;  // Clear cache
    buffersDirty_ = true;
    Refresh();
}

void LinePlot::SetTimeFormatFunction(std::function<wxString(int64_t, TimeResolution)> func)
{
    timeAxisConfig_.formatFunc = func;
    timeAxisConfig_.cachedTimestamp = -1;  // Clear cache
    Refresh();
}

// ---------------------------------------------------------------------------
// Event handlers
// ---------------------------------------------------------------------------
void LinePlot::OnPaint(wxPaintEvent& /*event*/)
{
    wxPaintDC dc(this);
    SetCurrent(*context_);

    if (!glInitialized_) {
        InitializeGL();
    }

    // Recompute axis limits from data when autoscale is enabled
    ApplyAutoscale();

    ComputeMargins();

    // Update VBOs if data changed
    if (buffersDirty_) {
        UpdateBuffers();
        buffersDirty_ = false;
    }

    glClearColor(bg_colour_.r, bg_colour_.g, bg_colour_.b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    RenderGrid();
    RenderSeries();
    if (axesEnabled_) {
        RenderAxes();
    }
    RenderOverlay();

    SwapBuffers();
}

void LinePlot::OnResize(wxSizeEvent& event)
{
    buffersDirty_ = true;
    Refresh();
    event.Skip();
}

void LinePlot::OnMouseMotion(wxMouseEvent& event) {
    mouseX_ = event.GetX();
    mouseY_ = event.GetY();
    Refresh();
    event.Skip();
}

void LinePlot::OnSysColourChanged(wxSysColourChangedEvent& event) {
    UpdateColoursFromSystem();
    Refresh();
    event.Skip();
}

void LinePlot::OnMouseWheel(wxMouseEvent& event)
{
    const wxPoint pos = event.GetPosition();

    // Use the internally computed x-axis rect
    if (xAxisRect_.Contains(pos))
    {
        if (onXAxisScroll)
            onXAxisScroll(event.GetWheelRotation());

        // Consume — do not propagate further
        return;
    }

    // Outside x-axis area: allow normal LinePlot handling
    event.Skip();
}

// ===========================================================================
// OpenGL initialization
// ===========================================================================
void LinePlot::InitializeGL()
{
    // Initialize GLEW
    glewExperimental = GL_TRUE;
    GLenum err = glewInit();
    if (err != GLEW_OK) {
        wxLogError("GLEW initialization failed: %s", glewGetErrorString(err));
        return;
    }

    // Enable MSAA
    glEnable(GL_MULTISAMPLE);
    
    // Enable blending for transparency
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glGenVertexArrays(1, &rectVAO_);
    glGenBuffers(1,      &rectVBO_);
    glBindVertexArray(rectVAO_);
    glBindBuffer(GL_ARRAY_BUFFER, rectVBO_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 12, nullptr, GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);

    // Compile shaders
    seriesShader_ = signal_ui::CompileShader(seriesShader,              solidColourFragmentShader);
    uniforms_.Resolve(seriesShader_);

    lineShader_ = signal_ui::CompileShader(simpleVertexShader,          solidColourFragmentShader);
    fillShader_ = signal_ui::CompileShader(simpleVertexShader,          solidColourFragmentShader);
    gridShader_ = signal_ui::CompileShader(simpleVertexShader,          solidColourFragmentShader);
    textShader_ = signal_ui::CompileShader(signal_ui::textVertexShader, signal_ui::textFragmentShader);

    // Initialize FreeType
    signal_ui::InitializeFreeTypeLib(ftLibrary_);
    signal_ui::InitializeFontAtlas(ftLibrary_, labelAtlas_, 64);
    signal_ui::InitializeFontAtlas(ftLibrary_, titleAtlas_, 64);

    glInitialized_ = true;
}

void LinePlot::CheckShaderCompileErrors(unsigned int shader, const std::string& type)
{
    GLint success;
    GLchar infoLog[1024];
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        glGetShaderInfoLog(shader, 1024, NULL, infoLog);
        wxLogError("Shader compilation error (%s): %s", type.c_str(), infoLog);
    }
}

void LinePlot::CheckProgramLinkErrors(unsigned int program)
{
    GLint success;
    GLchar infoLog[1024];
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) {
        glGetProgramInfoLog(program, 1024, NULL, infoLog);
        wxLogError("Shader program linking error: %s", infoLog);
    }
}

// ===========================================================================
// Layout helpers
// ===========================================================================
template <typename T>
T LinePlot::NiceStep(T lo, T hi) {
    // 1. Use long double for math to maintain precision for massive uint64_t
    using CalcType = long double;
    
    CalcType range = (hi > lo) ? static_cast<CalcType>(hi) - static_cast<CalcType>(lo) : 0;
    
    if (range <= 0) {
        return static_cast<T>(1);
    }

    // 2. Standard "Nice Number" Algorithm
    CalcType rawStep    = range / 6.0L;
    CalcType magLog     = std::floor(std::log10(rawStep));
    CalcType magnitude  = std::pow(10.0L, magLog);
    CalcType normalized = rawStep / magnitude;

    CalcType niceNorm;
    if      (normalized < 1.5L)  niceNorm = 1.0L;
    else if (normalized < 3.5L)  niceNorm = 2.0L;
    else if (normalized < 7.5L)  niceNorm = 5.0L;
    else                         niceNorm = 10.0L;

    CalcType result = niceNorm * magnitude;

    // 3. Branch based on Type
    if constexpr (std::is_integral_v<T>) {
        // For uint64_t (Timestamps): 
        // We don't want steps smaller than 1 tick.
        return static_cast<T>(std::max(1.0L, std::round(result)));
    } else {
        // For double/float (Y-Axis): 
        // Allow steps like 0.01, 0.0005, etc.
        return static_cast<T>(result);
    }
}

uint64_t LinePlot::EffectiveTickSpacingX() const
{
    return (tickSpacingX_ > 0.0) ? tickSpacingX_ : NiceStep(xMin_, xMax_);
}

float LinePlot::EffectiveTickSpacingY(int yAxisIndex) const
{
    if (yAxisIndex < 0 || yAxisIndex >= static_cast<int>(yAxes_.size())) {
        return 1.0;
    }
    
    const YAxisConfig& axis = yAxes_[yAxisIndex];
    return (tickSpacingY_ > 0.0) ? tickSpacingY_ : NiceStep(axis.min, axis.max);
}

float LinePlot::XAxisY() const
{
    switch (xAxisPos_)
    {
        case AxisPosition::Top:
            if (!yAxes_.empty()) return yAxes_[0].max;
            return 1.0;
        case AxisPosition::Bottom:
            if (!yAxes_.empty()) return yAxes_[0].min;
            return -1.0;
        default:
            return 0.0;
    }
}

float LinePlot::YAxisX(int yAxisIndex) const
{
    if (yAxisIndex < 0 || yAxisIndex >= static_cast<int>(yAxes_.size())) {
        return PlotRect().GetLeft();
    }

    constexpr float kAxisSpacing = 60.0f;
    FloatRect pr = PlotRect();

    // Count how many axes of the same position precede this one
    AxisPosition pos = yAxes_[yAxisIndex].position;
    int offset = 0;
    for (int i = 0; i < yAxisIndex; ++i) {
        if (yAxes_[i].enabled && yAxes_[i].position == pos) {
            ++offset;
        }
    }

    switch (pos)
    {
        case AxisPosition::Left:  return pr.GetLeft()  - offset * kAxisSpacing;
        case AxisPosition::Right: return pr.GetRight() + offset * kAxisSpacing;
        default:                  return pr.GetLeft();
    }
}

int LinePlot::YAxisScreenX(int yAxisIndex) const
{
    if (yAxisIndex < 0 || yAxisIndex >= static_cast<int>(yAxes_.size())) {
        return marginLeft_;
    }
    
    const YAxisConfig& axis = yAxes_[yAxisIndex];
    
    // Calculate width needed for this axis
    int axisWidth = 60;  // Base width for ticks + labels
    
    if (axis.position == AxisPosition::Left) {
        // Count how many left axes come before this one (by stack order)
        int offset = 0;
        for (int i = 0; i < static_cast<int>(yAxes_.size()); ++i) {
            if (i == yAxisIndex) break;
            if (yAxes_[i].position == AxisPosition::Left && 
                yAxes_[i].stackOrder < axis.stackOrder) {
                offset += 60;  // Width per axis
            }
        }
        return marginLeft_ - offset - axisWidth;
    } else {
        // Right side
        int w, h;
        const_cast<LinePlot*>(this)->GetClientSize(&w, &h);
        
        int offset = 0;
        for (int i = 0; i < static_cast<int>(yAxes_.size()); ++i) {
            if (i == yAxisIndex) break;
            if (yAxes_[i].position == AxisPosition::Right && 
                yAxes_[i].stackOrder < axis.stackOrder) {
                offset += 60;
            }
        }
        return w - marginRight_ + offset;
    }
}

// ---------------------------------------------------------------------------
// ApplyAutoscale
// ---------------------------------------------------------------------------
void LinePlot::ApplyAutoscale()
{
    constexpr double kPad = 0.05;

    // --- X-axis autoscale ---
    if (autoscaleX_) {
        bool    hasX = false;
        int64_t xlo  = std::numeric_limits<int64_t>::max();
        int64_t xhi  = std::numeric_limits<int64_t>::lowest();

        for (const auto& s : series_) {
            for (const auto& cv : s.chunks) {
                for (int64_t i = 0; i < cv.len; ++i) {
                    if (cv.xs[i] < xlo) xlo = cv.xs[i];
                    if (cv.xs[i] > xhi) xhi = cv.xs[i];
                }
                hasX = true;
            }
        }

        if (hasX) {
            int64_t range = xhi - xlo;
            if (range < 1) {
                xlo -= 500'000'000LL;
                xhi += 500'000'000LL;
                range = xhi - xlo;
            }
            int64_t pad = range / 20;
            xMin_ = xlo - pad;
            xMax_ = xhi + pad;
        }
    }

    // --- Y-axes autoscale (independent per axis) ---
    for (size_t axisIdx = 0; axisIdx < yAxes_.size(); ++axisIdx) {
        if (!yAxes_[axisIdx].autoscale) continue;

        bool   hasY = false;
        double ylo  = std::numeric_limits<double>::max();
        double yhi  = std::numeric_limits<double>::lowest();

        for (const auto& s : series_) {
            if (s.yAxisIndex != static_cast<int>(axisIdx)) continue;
            if (s.isBoolean) continue;

            for (const auto& cv : s.chunks) {
                for (int64_t i = 0; i < cv.len; ++i) {
                    const double v = static_cast<double>(cv.ys[i]);
                    if (!std::isfinite(v)) continue;
                    if (v < ylo) ylo = v;
                    if (v > yhi) yhi = v;
                    hasY = true;
                }
            }
        }

        if (hasY) {
            double range = yhi - ylo;
            if (range < 1e-12) {
                ylo -= 0.5;
                yhi += 0.5;
                range = 1.0;
            }
            yAxes_[axisIdx].min = ylo - kPad * range;
            yAxes_[axisIdx].max = yhi + kPad * range;
        }
    }
}

// ---------------------------------------------------------------------------
// ComputeMargins
// ---------------------------------------------------------------------------
void LinePlot::ComputeMargins()
{
    constexpr int edgePad  = 4;
    constexpr int labelGap = 3;
    constexpr int axisWidth = 60;  // Width per Y-axis

    // Count axes on each side
    int leftAxisCount = 0;
    int rightAxisCount = 0;
    
    for (const auto& axis : yAxes_) {
        if (!axis.enabled) continue;
        if (axis.position == AxisPosition::Left) {
            leftAxisCount++;
        } else if (axis.position == AxisPosition::Right) {
            rightAxisCount++;
        }
    }
    
    marginLeft_ = leftAxisCount * axisWidth + edgePad;
    marginRight_ = rightAxisCount * axisWidth + edgePad;
    
    int tickContrib = ticksEnabled_ ? tickLengthPx_ : 0;
    int gap = (ticksEnabled_ || labelsEnabled_) ? labelGap : 0;
    
    // Calculate X-axis label height
    int labelHeight = 20;  // Default single line
    if (timeAxisConfig_.enabled) {
        // Time labels always use 2 lines
        labelHeight = 2 * 14 + 5;  // 2 lines * font height + padding
    }
    
    int xBand = labelHeight + gap + tickContrib + edgePad;
    
    marginBottom_ = (xAxisPos_ == AxisPosition::Bottom || xAxisPos_ == AxisPosition::AtZero)
                    ? xBand : edgePad;
    marginTop_ = (xAxisPos_ == AxisPosition::Top) ? xBand : edgePad;

    // Title band
    if (!title_.IsEmpty())
        marginTop_ += 25 + edgePad;

    xAxisRect_ = wxRect(
            marginLeft_,                                    // starts after left y-axes
            GetClientSize().GetHeight() - marginBottom_,    // top edge of bottom band
            GetClientSize().GetWidth() - marginLeft_ - marginRight_,  // plot width only
            marginBottom_                                   // full x-axis band height
        );
}

void LinePlot::UpdateColoursFromSystem()
{
    wxColour bg = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW);
    wxColour fg = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
    wxColour grid = wxSystemSettings::GetColour(wxSYS_COLOUR_3DLIGHT);

    bg_colour_.r = bg.Red()   / 255.0f;
    bg_colour_.g = bg.Green() / 255.0f;
    bg_colour_.b = bg.Blue()  / 255.0f;

    grid_colour_.r = grid.Red()   / 255.0f;
    grid_colour_.g = grid.Green() / 255.0f;
    grid_colour_.b = grid.Blue()  / 255.0f;

    text_colour_.r = fg.Red()   / 255.0f;
    text_colour_.g = fg.Green() / 255.0f;
    text_colour_.b = fg.Blue()  / 255.0f;
}

LinePlot::FloatRect LinePlot::PlotRect() const
{
    int w, h;
    const_cast<LinePlot*>(this)->GetClientSize(&w, &h);
    
    FloatRect rect;
    rect.x = static_cast<float>(marginLeft_);
    rect.y = static_cast<float>(marginTop_);
    rect.width = static_cast<float>(w - marginLeft_ - marginRight_);
    rect.height = static_cast<float>(h - marginTop_ - marginBottom_);
    
    return rect;
}

void LinePlot::DataToScreen(uint64_t dx, double dy, int yAxisIndex, float& sx, float& sy) const
{
    // 1. Safety check for Y-axis
    if (yAxisIndex < 0 || yAxisIndex >= static_cast<int>(yAxes_.size())) {
        sx = sy = 0.0f;
        return;
    }
    const YAxisConfig& axis = yAxes_[yAxisIndex];
    FloatRect pr = PlotRect();

    // 2. Horizontal Projection (X-axis)
    // We calculate 'nx' (a 0.0 to 1.0 ratio of where dx sits between xMin and xMax)
    double nx = 0.0;
    uint64_t rangeX = xMax_ - xMin_;

    if (rangeX > 0) {
        if (dx >= xMin_) {
            // Standard case: subtract first as uint64, then convert to double
            nx = static_cast<double>(dx - xMin_) / static_cast<double>(rangeX);
        } else {
            // Underflow case: dx is to the left of the screen (e.g. older data)
            // We calculate the distance as a negative double
            nx = -static_cast<double>(xMin_ - dx) / static_cast<double>(rangeX);
        }
    }

    // 3. Vertical Projection (Y-axis)
    double rangeY = axis.max - axis.min;
    double ny = (rangeY != 0.0) ? (dy - axis.min) / rangeY : 0.0;

    // 4. Map to Screen Coordinates
    sx = pr.GetLeft() + static_cast<float>(nx) * pr.GetWidth();
    // Y is inverted: screen 0 is top, but data 0 is usually bottom
    sy = pr.GetTop() + (1.0f - static_cast<float>(ny)) * pr.GetHeight();
}

// ===========================================================================
// Buffer management
// ===========================================================================

void LinePlot::UpdateBuffers()
{
    UpdateSeriesBuffers();
    UpdateGridBuffers();
    UpdateAxisBuffers();
}

std::pair<float, float> LinePlot::ComputeYRange(
    const std::vector<ChunkView>& chunks,
    float                         paddingFraction)
{
    double ylo = std::numeric_limits<double>::max();
    double yhi = std::numeric_limits<double>::lowest();
    bool   any = false;

    auto accumulate = [&](double v)
    {
        if (!std::isfinite(v)) return;
        if (v < ylo) ylo = v;
        if (v > yhi) yhi = v;
        any = true;
    };
    auto accumulateInt = [&](double v)
    {
        // Integer values are always finite — skip isfinite check
        if (v < ylo) ylo = v;
        if (v > yhi) yhi = v;
        any = true;
    };

    for (const auto& cv : chunks)
    {
        for (int64_t i = 0; i < cv.len; ++i)
            accumulate(static_cast<double>(cv.ys[i]));
    }

    if (!any) return { -1.0f, 1.0f };

    if (ylo == yhi) { ylo -= 1.0; yhi += 1.0; }

    double pad = (yhi - ylo) * paddingFraction;
    return {
        static_cast<float>(ylo - pad),
        static_cast<float>(yhi + pad)
    };
}

void LinePlot::UpdateSeriesBuffers()
{
    size_t chunksNeeded = 0;
    for (const auto& s : series_)
        for (const auto& cv : s.chunks)
            if (cv.len > 0) ++chunksNeeded;

    while (seriesBuffers_.size() > chunksNeeded) {
        seriesBuffers_.back().Free();
        seriesBuffers_.pop_back();
    }

    size_t bufIdx = 0;

    for (const auto& s : series_) {
        float yMin = 0.f, yMax = 1.f;
        if (s.yAxisIndex >= 0 &&
            s.yAxisIndex < static_cast<int>(yAxes_.size()))
        {
            yMin = static_cast<float>(yAxes_[s.yAxisIndex].min);
            yMax = static_cast<float>(yAxes_[s.yAxisIndex].max);
        }

        for (const auto& cv : s.chunks) {
            if (cv.len == 0) continue;

            const int64_t xOffset    = xMin_;
            const GLsizei dataLen    = static_cast<GLsizei>(cv.len);
            const GLsizei uploadLen  = dataLen + (cv.hasBridge ? 1 : 0);

            if (bufIdx >= seriesBuffers_.size()) {
                SeriesBuffers sb;
                sb.Allocate(uploadLen * 2);
                seriesBuffers_.push_back(std::move(sb));
            }

            auto& sb = seriesBuffers_[bufIdx++];
            sb.EnsureCapacity(uploadLen);

            // --- Timestamp scratch ---
            sb.tScratch.resize(uploadLen);
            int64_t dstOffset = 0;
            if (cv.hasBridge) {
                sb.tScratch[0] = static_cast<float>(cv.bridgeTime - xOffset);
                dstOffset = 1;
            }
            for (int64_t i = 0; i < cv.len; ++i)
                sb.tScratch[dstOffset + i] = static_cast<float>(cv.xs[i] - xOffset);

            sb.UploadXs(uploadLen);

            // --- Value upload ---
            sb.UploadYs(
                uploadLen,
                cv.ys,
                dataLen,
                cv.hasBridge ? &cv.bridgeValue : nullptr);

            // Metadata
            sb.yAxisIndex = s.yAxisIndex;
            sb.colour     = s.colour;
            sb.isBoolean  = s.isBoolean;
            sb.xMin       = 0.f;
            sb.xMax       = static_cast<float>(xMax_ - xMin_);
            sb.yMin       = yMin;
            sb.yMax       = yMax;
        }
    }
}

void LinePlot::UpdateGridBuffers()
{
    if (!gridEnabled_) return;
    
    std::vector<float> vertices;
    FloatRect pr = PlotRect();
    
    // Vertical grid lines
    uint64_t stepX = (gridSpacingX_ > 0.0) ? gridSpacingX_ : NiceStep(xMin_, xMax_);
    if (stepX == 0) stepX = 1;
    uint64_t startX = ((xMin_ + stepX - 1) / stepX) * stepX;
    
    for (uint64_t x = startX; x <= xMax_; ) {
        float sx1, sy1, sx2, sy2;
        
        if (!yAxes_.empty()) {
            DataToScreen(x, yAxes_[0].min, 0, sx1, sy1);
            DataToScreen(x, yAxes_[0].max, 0, sx2, sy2);
        } else {
            // Use double for the ratio to prevent integer division (which would result in 0 or 1)
            double totalRange = static_cast<double>(xMax_ - xMin_);
            float t = (totalRange > 0) ? static_cast<float>((static_cast<double>(x - xMin_)) / totalRange) : 0.0f;
            
            sx1 = sx2 = pr.GetLeft() + t * pr.GetWidth();
            sy1 = pr.GetBottom();
            sy2 = pr.GetTop();
        }
        
        vertices.push_back(sx1);
        vertices.push_back(sy1);
        vertices.push_back(sx2);
        vertices.push_back(sy2);

        // 4. Safe increment: prevent uint64 overflow if xMax_ is near the end of uint64 range
        if (x > xMax_ - stepX) break; 
        x += stepX;
    }
    
    // Horizontal grid lines (using first Y-axis)
    if (!yAxes_.empty()) {
        double stepY = EffectiveTickSpacingY(0);
        double startY = std::ceil(yAxes_[0].min / stepY) * stepY;
        
        for (double y = startY; y <= yAxes_[0].max + stepY * 0.001; y += stepY) {
            float sx1, sy1, sx2, sy2;
            DataToScreen(xMin_, y, 0, sx1, sy1);
            DataToScreen(xMax_, y, 0, sx2, sy2);
            
            vertices.push_back(sx1);
            vertices.push_back(sy1);
            vertices.push_back(sx2);
            vertices.push_back(sy2);
        }
    }
    
    if (vertices.empty()) return;
    
    // Create/update grid VAO/VBO
    if (gridVAO_ == 0) {
        glGenVertexArrays(1, &gridVAO_);
        glGenBuffers(1, &gridVBO_);
    }
    
    glBindVertexArray(gridVAO_);
    glBindBuffer(GL_ARRAY_BUFFER, gridVBO_);
    glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(float), vertices.data(), GL_STATIC_DRAW);
    
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    
    glBindVertexArray(0);
}

void LinePlot::UpdateAxisBuffers()
{
    std::vector<float> vertices;
    FloatRect pr = PlotRect();

    // X-axis line
    float axY_screen;
    {
        float dummy;
        DataToScreen(xMin_, XAxisY(), 0, dummy, axY_screen);
    }

    vertices.push_back(pr.GetLeft());
    vertices.push_back(axY_screen);
    vertices.push_back(pr.GetRight());
    vertices.push_back(axY_screen);

    // Y-axes lines — screen-space X now accounts for per-axis offset
    for (size_t i = 0; i < yAxes_.size(); ++i) {
        if (!yAxes_[i].enabled) continue;

        float axX_screen = YAxisX(static_cast<int>(i)); // already screen-space

        vertices.push_back(axX_screen);
        vertices.push_back(pr.GetTop());
        vertices.push_back(axX_screen);
        vertices.push_back(pr.GetBottom());
    }

    if (vertices.empty()) return;

    if (axisVAO_ == 0) {
        glGenVertexArrays(1, &axisVAO_);
        glGenBuffers(1, &axisVBO_);
    }

    glBindVertexArray(axisVAO_);
    glBindBuffer(GL_ARRAY_BUFFER, axisVBO_);
    glBufferData(
        GL_ARRAY_BUFFER,
        vertices.size() * sizeof(float),
        vertices.data(),
        GL_DYNAMIC_DRAW
    );

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    glBindVertexArray(0);
}

// ===========================================================================
// Rendering functions
// ===========================================================================

void LinePlot::RenderFilledRect(float x, float y, float w, float h,
                                 const glm::vec4& colour,
                                 const glm::mat4& projection)
{
    // Vertices for two triangles forming a quad (top-left origin, y-down)
    float vertices[] = {
        // x        y
        x,          y,
        x + w,      y,
        x + w,      y + h,

        x,          y,
        x + w,      y + h,
        x,          y + h,
    };

    glUseProgram(fillShader_);

    GLint projLoc   = glGetUniformLocation(fillShader_, "u_proj");
    GLint colourLoc = glGetUniformLocation(fillShader_, "u_color");

    glUniformMatrix4fv(projLoc,   1, GL_FALSE, glm::value_ptr(projection));
    glUniform4f(colourLoc, colour.r, colour.g, colour.b, colour.a);

    glBindVertexArray(rectVAO_);
    glBindBuffer(GL_ARRAY_BUFFER, rectVBO_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glDrawArrays(GL_TRIANGLES, 0, 6);

    glBindVertexArray(0);
    glUseProgram(0);
}

void LinePlot::RenderRectOutline(float x, float y, float w, float h,
                                  const glm::vec3& colour,
                                  const glm::mat4& projection)
{
    // Four corners traversed as a closed line loop
    float vertices[] = {
        x,          y,
        x + w,      y,
        x + w,      y + h,
        x,          y + h,
    };

    glUseProgram(fillShader_);

    GLint projLoc   = glGetUniformLocation(fillShader_, "u_proj");
    GLint colourLoc = glGetUniformLocation(fillShader_, "u_color");

    glUniformMatrix4fv(projLoc,   1, GL_FALSE, glm::value_ptr(projection));
    glUniform4f(colourLoc, colour.r, colour.g, colour.b, 1.0f);

    glBindVertexArray(rectVAO_);
    glBindBuffer(GL_ARRAY_BUFFER, rectVBO_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    glDisable(GL_BLEND);

    glDrawArrays(GL_LINE_LOOP, 0, 4);

    glBindVertexArray(0);
    glUseProgram(0);
}

void LinePlot::RenderGrid()
{
    if (!gridEnabled_ || gridVAO_ == 0) return;
    
    glUseProgram(gridShader_);
    
    int w, h;
    GetClientSize(&w, &h);
    glViewport(0, 0, w, h);
    
    // Screen-space orthographic projection
    float proj[16] = {
        2.0f/w, 0.0f, 0.0f, 0.0f,
        0.0f, -2.0f/h, 0.0f, 0.0f,
        0.0f, 0.0f, -1.0f, 0.0f,
        -1.0f, 1.0f, 0.0f, 1.0f
    };
    
    glUniformMatrix4fv(glGetUniformLocation(gridShader_, "u_proj"), 1, GL_FALSE, proj);
    glUniform4f(glGetUniformLocation(gridShader_, "u_color"), 
                gridColourR_, gridColourG_, gridColourB_, 1.0f);
    
    glBindVertexArray(gridVAO_);
    
    // Get vertex count
    GLint size;
    glBindBuffer(GL_ARRAY_BUFFER, gridVBO_);
    glGetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_SIZE, &size);
    GLsizei count = size / (2 * sizeof(float));
    
    glDrawArrays(GL_LINES, 0, count);
    glBindVertexArray(0);
}

void LinePlot::RenderAxes()
{
    if (axisVAO_ == 0) return;
    
    glUseProgram(lineShader_);
    
    int w, h;
    GetClientSize(&w, &h);
    glViewport(0, 0, w, h);
    
    
    float proj[16] = {
        2.0f/w, 0.0f, 0.0f, 0.0f,
        0.0f, -2.0f/h, 0.0f, 0.0f,
        0.0f, 0.0f, -1.0f, 0.0f,
        -1.0f, 1.0f, 0.0f, 1.0f
    };
    
    glUniformMatrix4fv(glGetUniformLocation(lineShader_, "u_proj"), 1, GL_FALSE, proj);
    glUniform4f(glGetUniformLocation(lineShader_, "u_color"),
                gridColourR_, gridColourG_, gridColourB_, 1.0f);
    
    glBindVertexArray(axisVAO_);
    
    GLint size;
    glBindBuffer(GL_ARRAY_BUFFER, axisVBO_);
    glGetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_SIZE, &size);
    GLsizei count = size / (2 * sizeof(float));
    
    glDrawArrays(GL_LINES, 0, count);
    glBindVertexArray(0);
}

void LinePlot::RenderSeries()
{
    glUseProgram(seriesShader_);

    int w, h;
    GetClientSize(&w, &h);
    glViewport(0, 0, w, h);
    
    float proj[16] = {
        2.0f/w, 0.0f, 0.0f, 0.0f,
        0.0f, -2.0f/h, 0.0f, 0.0f,
        0.0f, 0.0f, -1.0f, 0.0f,
        -1.0f, 1.0f, 0.0f, 1.0f
    };

    // Upload shared projection — identical matrix used by axis shader
    glUniformMatrix4fv(glGetUniformLocation(seriesShader_, "uProjection"), 1, GL_FALSE, proj);

    const FloatRect pr = PlotRect();

    glUniform1f(uniforms_.scrLeft,   pr.GetLeft());
    glUniform1f(uniforms_.scrRight,  pr.GetRight());
    glUniform1f(uniforms_.scrTop,    pr.GetTop());
    glUniform1f(uniforms_.scrBottom, pr.GetBottom());

    for (const auto& sb : seriesBuffers_) {
        if (!sb.valid() || sb.vertexCount == 0) continue;

        glUniform1f(uniforms_.xMin, sb.xMin);
        glUniform1f(uniforms_.xMax, sb.xMax);
        glUniform1f(uniforms_.yMin, sb.yMin);
        glUniform1f(uniforms_.yMax, sb.yMax);

        glUniform4f(glGetUniformLocation(seriesShader_, "u_color"),
                    sb.colour.r, sb.colour.g, sb.colour.b, 1.0f);

        glBindVertexArray(sb.vao);
        glDrawArrays(GL_LINE_STRIP, 0, sb.vertexCount);
        glBindVertexArray(0);
    }

    glUseProgram(0);
}

void LinePlot::RenderOverlay()
{
    if (!labelAtlas_.textureID || !titleAtlas_.textureID) return;

    int w, h;
    GetClientSize(&w, &h);
    if (w <= 0 || h <= 0) return;

    glViewport(0, 0, w, h);

    // Build overlay projection once — maps pixel coords to clip space
    glm::mat4 overlay = glm::ortho(0.0f, static_cast<float>(w),
                                    static_cast<float>(h), 0.0f,
                                   -1.0f, 1.0f);

    // ── 1. Title ─────────────────────────────────────────────────────
    if (!title_.IsEmpty()) {
        std::string titleStr = title_.ToStdString();
        glm::vec2 ts = MeasureText(titleStr, titleAtlas_, titleFontSize_);
        float tx = (w - ts.x) * 0.5f;
        float ty = 10.0f;

        RenderText(titleStr, tx, ty, titleFontSize_,
                   titleAtlas_, textShader_,
                   {text_colour_.r, text_colour_.g, text_colour_.b},
                   overlay);
    }

    // ── 2. X-axis labels ────────────────────────────────────────────
    if (labelsEnabled_) {
        double stepX = EffectiveTickSpacingX();
        uint64_t uStepX = static_cast<uint64_t>(std::max(1.0, stepX));
        if (uStepX == 0) uStepX = 1;

        uint64_t start = 0;
        if (xMin_ > 0) {
            start = ((xMin_ + uStepX - 1) / uStepX) * uStepX;
        }

        double axY = XAxisY();
        float lineHeight = labelFontSize_ + 2.0f;

        for (uint64_t x = start; x <= xMax_; x += uStepX) {
            wxString label;

            if (timeAxisConfig_.enabled) {
                TimeResolution resolution =
                    TimeAxisConfig::DetermineResolution(xMax_ - xMin_);

                if (x == timeAxisConfig_.cachedTimestamp &&
                    resolution == timeAxisConfig_.cachedResolution) {
                    label = timeAxisConfig_.cachedResult;
                } else {
                    label = timeAxisConfig_.formatFunc(x, resolution);
                    timeAxisConfig_.cachedTimestamp  = x;
                    timeAxisConfig_.cachedResolution = resolution;
                    timeAxisConfig_.cachedResult     = label;
                }

                // Split multi-line labels
                wxArrayString lines;
                size_t pos = 0, found;
                while ((found = label.find('\n', pos)) != wxString::npos) {
                    lines.Add(label.Mid(pos, found - pos));
                    pos = found + 1;
                }
                if (pos < label.length()) {
                    lines.Add(label.Mid(pos));
                }

                float sx, sy;
                DataToScreen(x, axY, 0, sx, sy);
                float startY = sy + 15.0f;

                for (size_t lineIdx = 0; lineIdx < lines.GetCount(); ++lineIdx) {
                    if (lines[lineIdx].IsEmpty()) continue;

                    std::string lineStr = lines[lineIdx].ToStdString();
                    glm::vec2 lineSize = MeasureText(lineStr, labelAtlas_, labelFontSize_);
                    float lineX = sx - lineSize.x * 0.5f;
                    float lineY = startY + (lineIdx * lineHeight);

                    RenderText(lineStr, lineX, lineY, labelFontSize_,
                               labelAtlas_, textShader_,
                               {text_colour_.r, text_colour_.g, text_colour_.b},
                               overlay);
                }
            }
            else {
                // Numeric fallback
                label = wxString::Format("%llu", x);
                std::string labelStr = label.ToStdString();

                float sx, sy;
                DataToScreen(x, axY, 0, sx, sy);

                glm::vec2 labelSize = MeasureText(labelStr, labelAtlas_, labelFontSize_);

                RenderText(labelStr, sx - labelSize.x * 0.5f, sy + 15.0f,
                           labelFontSize_,
                           labelAtlas_, textShader_,
                           {text_colour_.r, text_colour_.g, text_colour_.b},
                           overlay);
            }
        }
    }

    // ── 3. Y-axis labels ────────────────────────────────────────────
    if (labelsEnabled_) {
        for (size_t axisIdx = 0; axisIdx < yAxes_.size(); ++axisIdx) {
            const YAxisConfig& axis = yAxes_[axisIdx];
            if (!axis.enabled) continue;

            double stepY = EffectiveTickSpacingY(static_cast<int>(axisIdx));
            if (stepY <= 0.0) continue;

            // Screen-space X for this axis — no longer routed through DataToScreen
            float axX_screen = YAxisX(static_cast<int>(axisIdx));

            double start = std::ceil(axis.min / stepY) * stepY;

            for (double y = start; y <= axis.max + stepY * 0.001; y += stepY) {
                std::ostringstream oss;
                oss << std::fixed << std::setprecision(labelPrecision_) << y;
                std::string labelStr = oss.str();

                // Only use DataToScreen for the Y screen coordinate
                float dummy, sy;
                DataToScreen(xMin_, y, static_cast<int>(axisIdx), dummy, sy);

                glm::vec2 labelSize = MeasureText(labelStr, labelAtlas_, labelFontSize_);

                float lx = (axis.position == AxisPosition::Left)
                        ? (axX_screen - labelSize.x - 10.0f)
                        : (axX_screen + 10.0f);

                RenderText(labelStr, lx, sy - labelSize.y * 0.5f,
                        labelFontSize_,
                        labelAtlas_, textShader_,
                        {text_colour_.r, text_colour_.g, text_colour_.b},
                        overlay);
            }
        }
    }

    // ── 4. Legend ────────────────────────────────────────────────────
    if (legendEnabled_) {
        RenderLegend(overlay, w, h);
    }
}

void LinePlot::RenderLegend(const glm::mat4& overlay, int w, int h)
{
    if (series_.empty()) return;

    // ── Measure legend box dimensions ─────────────────────────────
    const float paddingX      = 10.0f;
    const float paddingY      = 8.0f;
    const float swatchWidth   = 16.0f;
    const float swatchHeight  = 3.0f;
    const float swatchTextGap = 6.0f;
    const float anchorMargin  = 12.0f;

    // Use CalculateLayout to get accurate row height from font metrics
    // rather than a raw fontSize estimate
    signal_ui::TextLayout sampleLayout = signal_ui::CalculateLayout("Ag", labelAtlas_, labelFontSize_,
                                              signal_ui::TextAlign::LEFT,
                                              signal_ui::TextAlign::MIDDLE);
    const float itemSpacingY = sampleLayout.size.y + 6.0f;

    // Count visible entries and find widest label
    int   visibleCount = 0;
    float maxTextWidth = 0.0f;

    for (const auto& s : series_) {
        if (s.name.IsEmpty()) continue;

        signal_ui::TextLayout tl = signal_ui::CalculateLayout(s.name.ToStdString(), labelAtlas_,
                                        labelFontSize_,
                                        signal_ui::TextAlign::LEFT,
                                        signal_ui::TextAlign::MIDDLE);
        maxTextWidth = std::max(maxTextWidth, tl.size.x);
        ++visibleCount;
    }

    if (visibleCount == 0) return;

    float boxW = paddingX * 2.0f + swatchWidth + swatchTextGap + maxTextWidth;
    float boxH = paddingY * 2.0f + visibleCount * itemSpacingY - 6.0f;

    // ── Plot area bounds (derived from ComputeMargins()) ──────────
    const float plotLeft   = static_cast<float>(marginLeft_);
    const float plotTop    = static_cast<float>(marginTop_);
    const float plotRight  = static_cast<float>(w - marginRight_);
    const float plotBottom = static_cast<float>(h - marginBottom_);

    // ── Resolve box origin from legend position ───────────────────
    float boxX = 0.0f;
    float boxY = 0.0f;

    switch (legendPos_) {
        case LegendPosition::TopLeft:
            boxX = plotLeft   + anchorMargin;
            boxY = plotTop    + anchorMargin;
            break;

        case LegendPosition::TopRight:
            boxX = plotRight  - anchorMargin - boxW;
            boxY = plotTop    + anchorMargin;
            break;

        case LegendPosition::BottomLeft:
            boxX = plotLeft   + anchorMargin;
            boxY = plotBottom - anchorMargin - boxH;
            break;

        case LegendPosition::BottomRight:
            boxX = plotRight  - anchorMargin - boxW;
            boxY = plotBottom - anchorMargin - boxH;
            break;
    }

    // ── Background ────────────────────────────────────────────────
    RenderFilledRect(boxX, boxY, boxW, boxH,
                     {legend_bg_colour_.r,
                      legend_bg_colour_.g,
                      legend_bg_colour_.b,
                      legend_bg_alpha_},
                     overlay);

    // ── Border ────────────────────────────────────────────────────
    RenderRectOutline(boxX, boxY, boxW, boxH,
                      {legend_border_colour_.r,
                       legend_border_colour_.g,
                       legend_border_colour_.b},
                      overlay);

    // ── Entries ───────────────────────────────────────────────────
    float cursorY = boxY + paddingY;

    for (const auto& s : series_) {
        if (s.name.IsEmpty()) continue;

        // Vertical centre of this row — swatch and text both anchor here
        float rowCentreY = cursorY + (itemSpacingY * 0.5f);

        // ── Colour swatch ─────────────────────────────────────────
        float swatchX = boxX + paddingX;
        float swatchY = rowCentreY - (swatchHeight * 0.5f);

        RenderFilledRect(swatchX, swatchY, swatchWidth, swatchHeight,
                         {s.colour.r, s.colour.g, s.colour.b, 1.0f},
                         overlay);

        // ── Series name, vertically centred on swatch ─────────────
        // TextAlign::MIDDLE shifts offset.y so the font's visual
        // midpoint (between ascender and descender) lands on rowCentreY
        std::string nameStr = s.name.ToStdString();
        signal_ui::TextLayout layout = signal_ui::CalculateLayout(nameStr, labelAtlas_, labelFontSize_,
                                            signal_ui::TextAlign::LEFT,
                                            signal_ui::TextAlign::MIDDLE);

        float textX = swatchX + swatchWidth + swatchTextGap + layout.offset.x;
        float textY = rowCentreY                            + layout.offset.y;

        RenderText(nameStr, textX, textY, labelFontSize_,
                   labelAtlas_, textShader_,
                   {text_colour_.r, text_colour_.g, text_colour_.b},
                   overlay);

        cursorY += itemSpacingY;
    }
}

void LinePlot::RenderCrosshair()
{
    // Crosshair implementation if needed
}