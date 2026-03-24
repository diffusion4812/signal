#ifndef TIME_PLOT_H
#define TIME_PLOT_H

#include "line_plot.h"
#include <wx/wx.h>
#include <wx/timer.h>
#include <vector>
#include <string>
#include <deque>

// ---------------------------------------------------------------------------
// TimePlot – a self-updating, scrolling real-time plot panel.
//
// Responsibilities
//   • Owns a LinePlot and lays it out with a Play/Pause toolbar button.
//   • Holds an append-only ring buffer for every registered data stream.
//   • When playing, a wxTimer fires at ~30 fps.  On each tick the visible
//     window [now − windowSec, now] is sliced from the buffers and pushed
//     into the LinePlot, with x-coordinates shifted so the labels always
//     read [0 … windowSec].
//   • When paused the timer stops; the plot freezes at the last rendered
//     frame.  All incoming data is still buffered, so pressing Play again
//     jumps straight to the live edge.
//
// Coordinate convention
//   Internal timestamps are arbitrary monotonic floats (e.g. seconds since
//   program start).  Before they are handed to LinePlot they are rebased so
//   that the left edge of the visible window maps to x = 0.
//
// Boolean streams
//   Registered with AddBooleanSeries.  Internally they share the same
//   (time, value) buffer as continuous streams.  The boolean band is
//   positioned in the lower portion of the y-axis.  If there are also
//   continuous streams, the y-range is split: [yMin, boolTop] for booleans,
//   [boolTop, yMax] for continuous data.  The split point and per-lane
//   geometry are recomputed on every tick.
// ---------------------------------------------------------------------------
class TimePlot : public wxPanel
{
public:
    // -----------------------------------------------------------------------
    // Construction
    // -----------------------------------------------------------------------
    /// @param parent       Parent window.
    /// @param windowSec    Number of seconds visible at once (x-axis span).
    /// @param yMin         Bottom of the continuous-data y-range.
    /// @param yMax         Top   of the continuous-data y-range.
    TimePlot(wxWindow* parent, float windowSec, float yMin, float yMax);
    ~TimePlot();

    // -----------------------------------------------------------------------
    // Stream registration  (must be called before any AppendSample)
    // -----------------------------------------------------------------------
    /// Register a continuous (polyline) stream.  Returns its index.
    int AddContinuousSeries(const wxString& name, float r, float g, float b);

    /// Register a boolean (rectangle) stream.  Returns its index.
    int AddBooleanSeries(const wxString& name, float r, float g, float b);

    // -----------------------------------------------------------------------
    // Data ingestion
    // -----------------------------------------------------------------------
    /// Push one (time, value) sample into the stream at the given index.
    /// `time` is a monotonic float (e.g. seconds since epoch or program start).
    /// Must be called from the main (GUI) thread.
    void AppendSample(int seriesIndex, float time, float value);

    // -----------------------------------------------------------------------
    // Playback control
    // -----------------------------------------------------------------------
    void SetPlaying(bool playing);
    bool IsPlaying() const;

    // -----------------------------------------------------------------------
    // Autoscale
    // -----------------------------------------------------------------------
    /// When enabled the continuous-stream y-range floats to fit the visible
    /// data each tick.  The x-axis is always [0, windowSec] (the scrolling
    /// window), so there is no SetAutoscaleX on TimePlot.
    void SetAutoscaleY(bool on);

    // -----------------------------------------------------------------------
    // Accessors
    // -----------------------------------------------------------------------
    /// Direct access to the underlying LinePlot for further configuration
    /// (title, legend, grid, ticks, …).  Call after construction, before the
    /// first paint.
    LinePlot* GetPlot();

private:
    // -----------------------------------------------------------------------
    // Internal stream descriptor
    // -----------------------------------------------------------------------
    struct Stream
    {
        std::deque<float> times;   // monotonic timestamps
        std::deque<float> values;  // corresponding sample values
        wxString          name;
        float             colorR, colorG, colorB;
        bool              isBoolean;
    };

    // -----------------------------------------------------------------------
    // Widgets & layout
    // -----------------------------------------------------------------------
    LinePlot*         plot_;
    wxButton*         playPauseBtn_;
    wxTimer*          timer_;

    // -----------------------------------------------------------------------
    // State
    // -----------------------------------------------------------------------
    std::vector<Stream> streams_;
    float               windowSec_;    // visible x-span in seconds
    float               yMin_, yMax_;  // continuous-data y-range (base values)
    bool                playing_;
    bool                autoscaleY_;   // recompute continuous y-range each tick
    float               latestTime_;   // highest timestamp seen so far

    // -----------------------------------------------------------------------
    // Event handlers
    // -----------------------------------------------------------------------
    void OnPlayPause(wxCommandEvent& event);
    void OnTimer(wxTimerEvent& event);

    // -----------------------------------------------------------------------
    // Helpers
    // -----------------------------------------------------------------------
    /// Slice buffers, rebase x, build PlotSeries vector, push to LinePlot.
    void UpdatePlot();

    /// Remove samples older than the current visible window from all buffers.
    void PruneOldSamples();

    wxDECLARE_EVENT_TABLE();
};

#endif // TIME_PLOT_H