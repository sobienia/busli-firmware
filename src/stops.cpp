// ─────────────────────────────────────────────────────────────────────────────
// stops.cpp — transit stop table (Settings or built-in defaults)
// ─────────────────────────────────────────────────────────────────────────────

#include "app.h"

// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║  HARDCODED STOP DEFAULTS — used when NVS has no saved stops               ║
// ╚═══════════════════════════════════════════════════════════════════════════╝

static const struct { const char* l; const char* n; const char* li; const char* di; }
DEFAULT_STOPS[] = {
    { "ETH Hönggerberg",  "ETH Hönggerberg",             "",     "Triemli,Milchbuck,Bucheggplatz" },
    { "Gasometerbrücke",  "Schlieren, Gasometerbrücke",  "2,20", ""  },
    { "Letzipark",        "Zürich, Letzipark",            "",     ""  },
    { "Letzipark West",   "Zürich, Letzipark West",       "",     ""  },
};
static const int N_DEFAULT_STOPS = sizeof(DEFAULT_STOPS) / sizeof(DEFAULT_STOPS[0]);

// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║  DYNAMIC STOP TABLE — built at boot from NVS or defaults                  ║
// ╚═══════════════════════════════════════════════════════════════════════════╝

static StopEntry        g_stop_entries[8];
static std::vector<String>       g_line_strs[8];
static std::vector<String>       g_dir_strs[8];
static std::vector<const char*>  g_line_ptrs[8];
static std::vector<const char*>  g_dir_ptrs[8];
StopConfig       g_stop_configs[8];
int              g_num_stops = 0;

static void split_csv(const String& csv, std::vector<String>& out) {
    out.clear();
    int start = 0;
    for (int i = 0; i <= (int)csv.length(); i++) {
        if (i == (int)csv.length() || csv[i] == ',') {
            String tok = csv.substring(start, i);
            tok.trim();
            if (tok.length() > 0) out.push_back(tok);
            start = i + 1;
        }
    }
}

// Call after g_stop_entries[] and g_num_stops are set.
static void build_stop_configs() {
    for (int i = 0; i < g_num_stops; i++) {
        StopEntry& e = g_stop_entries[i];

        split_csv(e.lines_csv, g_line_strs[i]);
        g_line_ptrs[i].clear();
        for (auto& s : g_line_strs[i]) g_line_ptrs[i].push_back(s.c_str());

        split_csv(e.dirs_csv, g_dir_strs[i]);
        g_dir_ptrs[i].clear();
        for (auto& s : g_dir_strs[i]) g_dir_ptrs[i].push_back(s.c_str());

        g_stop_configs[i].label            = e.label.c_str();
        g_stop_configs[i].station          = e.station.c_str();
        g_stop_configs[i].line_filter      = g_line_ptrs[i].empty() ? nullptr : g_line_ptrs[i].data();
        g_stop_configs[i].line_count       = (int)g_line_ptrs[i].size();
        g_stop_configs[i].direction_filter = g_dir_ptrs[i].empty() ? nullptr : g_dir_ptrs[i].data();
        g_stop_configs[i].direction_count  = (int)g_dir_ptrs[i].size();
    }
}

void setup_stops() {
    int n = config_load_stops(g_stop_entries);
    if (n > 0) {
        g_num_stops = n;
        Log.printf("[Stops] Loaded %d stops from NVS\n", n);
    } else {
        g_num_stops = N_DEFAULT_STOPS;
        for (int i = 0; i < g_num_stops; i++) {
            g_stop_entries[i].label     = DEFAULT_STOPS[i].l;
            g_stop_entries[i].station   = DEFAULT_STOPS[i].n;
            g_stop_entries[i].lines_csv = DEFAULT_STOPS[i].li;
            g_stop_entries[i].dirs_csv  = DEFAULT_STOPS[i].di;
        }
        Log.printf("[Stops] Using %d hardcoded defaults\n", g_num_stops);
    }
    build_stop_configs();
}
