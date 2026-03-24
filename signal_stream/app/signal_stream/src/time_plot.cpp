#include "time_plot.h"
#include <algorithm>

// ---------------------------------------------------------------------------
// Timer ID & refresh rate
// ---------------------------------------------------------------------------
static constexpr int   kTimerID       = 1001;
static constexpr int   kTimerIntervalMs = 5; // ms

// ---------------------------------------------------------------------------
// Boolean-lane layout constants (in data-space units, relative to yMin_)
// ---------------------------------------------------------------------------
// Each boolean lane occupies a fixed height.  A small gap separates lanes.
// The entire boolean band sits at the bottom of the y-range.
static constexpr float kBoolLaneHeight  = 0.6f;   // per-lane height
static constexpr float kBoolLaneGap     = 0.15f;  // gap between lanes
static constexpr float kBoolBandGap     = 0.25f;  // gap above the bool band

// ---------------------------------------------------------------------------
// Event table
// ---------------------------------------------------------------------------
wxBEGIN_EVENT_TABLE(TimePlot, wxPanel)
    EVT_BUTTON(wxID_ANY,        TimePlot::OnPlayPause)
    EVT_TIMER(kTimerID,         TimePlot::OnTimer)
wxEND_EVENT_TABLE()

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------
TimePlot::TimePlot(wxWindow* parent, float windowSec, float yMin, float yMax)
    : wxPanel(parent, wxID_ANY),
      plot_(nullptr),
      playPauseBtn_(nullptr),
      timer_(nullptr),
      windowSec_(windowSec),
      yMin_(yMin), yMax_(yMax),
      playing_(true),
      autoscaleY_(false),
      latestTime_(0.f)
{
    // --- Layout: toolbar on top, plot below --------------------------------
    wxBoxSizer* vSizer = new wxBoxSizer(wxVERTICAL);

    // Toolbar row
    wxBoxSizer* toolSizer = new wxBoxSizer(wxHORIZONTAL);
    playPauseBtn_ = new wxButton(this, wxID_ANY, "Pause");
    toolSizer->Add(playPauseBtn_, 0, wxALIGN_CENTRE_VERTICAL | wxALL, 4);
    vSizer->Add(toolSizer, 0, wxEXPAND);

    // The plot widget fills the rest
    plot_ = new LinePlot(this);
    plot_->SetTitle("");
    plot_->SetLegendEnabled(true);
    plot_->SetLegendPosition(LegendPosition::TopRight);
    vSizer->Add(plot_, 1, wxEXPAND);

    SetSizer(vSizer);
    Layout();

    // Start the refresh timer
    timer_ = new wxTimer(this, kTimerID);
    timer_->Start(kTimerIntervalMs);
}

TimePlot::~TimePlot()
{
    if (timer_)
    {
        timer_->Stop();
        delete timer_;
    }
    // plot_ and playPauseBtn_ are owned by the wx widget tree; destroyed
    // automatically when this panel is destroyed.
}

// ---------------------------------------------------------------------------
// Stream registration
// ---------------------------------------------------------------------------
int TimePlot::AddContinuousSeries(const wxString& name, float r, float g, float b)
{
    Stream s;
    s.name = name;
    s.colorR = r; s.colorG = g; s.colorB = b;
    s.isBoolean = false;
    streams_.push_back(s);
    return static_cast<int>(streams_.size()) - 1;
}

int TimePlot::AddBooleanSeries(const wxString& name, float r, float g, float b)
{
    Stream s;
    s.name = name;
    s.colorR = r; s.colorG = g; s.colorB = b;
    s.isBoolean = true;
    streams_.push_back(s);
    return static_cast<int>(streams_.size()) - 1;
}

// ---------------------------------------------------------------------------
// Data ingestion
// ---------------------------------------------------------------------------
void TimePlot::AppendSample(int seriesIndex, float time, float value)
{
    if (seriesIndex < 0 || seriesIndex >= static_cast<int>(streams_.size()))
        return;

    streams_[seriesIndex].times.push_back(time);
    streams_[seriesIndex].values.push_back(value);

    if (time > latestTime_)
        latestTime_ = time;
}

// ---------------------------------------------------------------------------
// Playback control
// ---------------------------------------------------------------------------
void TimePlot::SetPlaying(bool playing)
{
    playing_ = playing;
    if (playing_)
    {
        playPauseBtn_->SetLabel("Pause");
        timer_->Start(kTimerIntervalMs);
    }
    else
    {
        playPauseBtn_->SetLabel("Play");
        timer_->Stop();
    }
}

bool TimePlot::IsPlaying() const { return playing_; }

// ---------------------------------------------------------------------------
// Autoscale
// ---------------------------------------------------------------------------
void TimePlot::SetAutoscaleY(bool on)
{
    autoscaleY_ = on;
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------
LinePlot* TimePlot::GetPlot() { return plot_; }

// ---------------------------------------------------------------------------
// Event handlers
// ---------------------------------------------------------------------------
void TimePlot::OnPlayPause(wxCommandEvent& /*event*/)
{
    SetPlaying(!playing_);
}

void TimePlot::OnTimer(wxTimerEvent& /*event*/)
{
    if (!playing_) return;
    UpdatePlot();
}

// ---------------------------------------------------------------------------
// UpdatePlot – the heart of the live-scrolling logic.
//
// Algorithm
//   1. Determine the visible window [xLeft, xRight] in raw timestamps.
//   2. For each stream, binary-search for the first sample inside the window
//      and copy [first … end] into a PlotSeries.  Shift all x values so
//      xLeft maps to 0 (labels read 0 … windowSec).
//   3. Compute boolean lane geometry: count boolean streams, stack them
//      from yMin_ upward.  If there are also continuous streams, shrink
//      their effective yMin to sit above the boolean band.
//   4. Push the series vector and adjusted axes limits into LinePlot.
// ---------------------------------------------------------------------------
void TimePlot::UpdatePlot()
{
    if (streams_.empty()) return;

    // Trim samples that are well outside the visible window.
    PruneOldSamples();

    float xRight = latestTime_;
    float xLeft  = xRight - windowSec_;

    // --- Count boolean and continuous streams ------------------------------
    int numBool = 0;
    for (const auto& s : streams_)
        if (s.isBoolean) ++numBool;

    // --- Compute boolean band geometry -------------------------------------
    // The boolean band occupies [yMin_, yMin_ + boolBandTop).
    // Each lane i (bottom-to-top) sits at:
    //   base = yMin_ + i * (kBoolLaneHeight + kBoolLaneGap)
    float boolBandTop = yMin_;   // top edge of the entire boolean band
    if (numBool > 0)
    {
        boolBandTop = yMin_ + numBool * kBoolLaneHeight
                            + (numBool - 1) * kBoolLaneGap
                            + kBoolBandGap;
    }

    // --- Determine the effective y-range for continuous data ---------------
    // When autoscaleY_ is on we scan the visible portion of every continuous
    // stream to find the actual data extent, then apply 5 % padding.
    // When it is off we use the fixed [boolBandTop … yMax_] (or [yMin_ … yMax_]
    // when there are no boolean streams).
    float contYMin, contYMax;

    if (autoscaleY_)
    {
        constexpr float kPad = 0.05f;
        bool hasData = false;
        float lo =  1e30f, hi = -1e30f;

        for (const auto& stream : streams_)
        {
            if (stream.isBoolean) continue;
            for (size_t i = 0; i < stream.times.size(); ++i)
            {
                if (stream.times[i] < xLeft) continue;   // outside window
                float v = stream.values[i];
                if (v < lo) lo = v;
                if (v > hi) hi = v;
                hasData = true;
            }
        }

        if (hasData)
        {
            float range = hi - lo;
            if (range < 1e-12f) { lo -= 0.5f; hi += 0.5f; range = 1.f; }
            contYMin = lo - kPad * range;
            contYMax = hi + kPad * range;
        }
        else
        {
            // No continuous data yet — fall back to constructor values.
            contYMin = (numBool > 0) ? boolBandTop : yMin_;
            contYMax = yMax_;
        }

        // Boolean bands must still sit below the continuous range.
        // If the auto-computed contYMin has drifted below boolBandTop,
        // clamp it up.
        if (numBool > 0 && contYMin < boolBandTop)
            contYMin = boolBandTop;
    }
    else
    {
        contYMin = (numBool > 0) ? boolBandTop : yMin_;
        contYMax = yMax_;
    }

    // The overall y-range handed to LinePlot spans from the very bottom
    // (yMin_, where boolean lanes live) to contYMax.
    float effectiveYMin = (numBool > 0) ? yMin_ : contYMin;
    float effectiveYMax = contYMax;

    // --- Build series vector -----------------------------------------------
    std::vector<PlotSeries> plotSeries;
    plotSeries.reserve(streams_.size());

    int boolIndex = 0;   // running index among boolean streams only

    for (const auto& stream : streams_)
    {
        PlotSeries ps;
        ps.name   = stream.name;
        ps.isBoolean    = stream.isBoolean;
        ps.boolBaseY    = 0.f;
        ps.boolBarHeight = 0.f;

        if (stream.isBoolean)
        {
            // Position this lane
            ps.boolBaseY    = yMin_ + boolIndex * (kBoolLaneHeight + kBoolLaneGap);
            ps.boolBarHeight = kBoolLaneHeight;
            ++boolIndex;
        }

        // --- Slice the visible window from the deques -----------------------
        // Find the first sample with time >= xLeft.
        // For boolean series we also want the last sample *before* xLeft so
        // that a rectangle that started before the window edge is still drawn.
        size_t startIdx = 0;
        if (!stream.times.empty())
        {
            // Linear scan from the front (deque; binary search would need
            // random access which deque technically supports but the data
            // is already pruned so it's short).
            startIdx = stream.times.size();   // default: nothing visible
            for (size_t i = 0; i < stream.times.size(); ++i)
            {
                if (stream.times[i] >= xLeft)
                {
                    // For booleans, back up one sample so the rect that
                    // spans across xLeft is still drawn.
                    startIdx = (stream.isBoolean && i > 0) ? i - 1 : i;
                    break;
                }
            }
        }

        // Copy visible slice, rebasing x so xLeft → 0.
        for (size_t i = startIdx; i < stream.times.size(); ++i)
        {
            float t = stream.times[i] - xLeft;   // rebase
            float v = stream.values[i];

            // For continuous streams, clamp value to [contYMin, contYMax].
            if (!stream.isBoolean)
                v = std::max(contYMin, std::min(contYMax, v));

            ps.xs.push_back(t);
            ps.ys.push_back(v);
        }

        plotSeries.push_back(std::move(ps));
    }

    // --- Push to LinePlot --------------------------------------------------
    plot_->SetSeries(plotSeries);
}

// ---------------------------------------------------------------------------
// PruneOldSamples – keeps only samples within [now - 2*windowSec, ∞).
// The 2× factor gives a safety margin so we never accidentally drop data
// that is still within the visible window at the next tick.
// ---------------------------------------------------------------------------
void TimePlot::PruneOldSamples()
{
    float cutoff = latestTime_ - 2.f * windowSec_;
    for (auto& stream : streams_)
    {
        while (!stream.times.empty() && stream.times.front() < cutoff)
        {
            stream.times.pop_front();
            stream.values.pop_front();
        }
    }
}