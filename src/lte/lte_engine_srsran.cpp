// CellScope LTE engine implementation for macOS/Emscripten.
//
// This backend keeps the LTE path on plain srsRAN 4G primitives only:
// cell search, MIB/SIB1 decode, and common-channel PDCCH/PDSCH inspection.
// It deliberately avoids the Falcon/LTESniffer stack so the macOS build can
// stay on the supported upstream srsRAN path.
#include "lte_engine.h"
#include "lte_diag.h"

#include "srsran/srsran.h"
#include "srsran/asn1/rrc/bcch_msg.h"
#include "srsran/phy/ue/ue_cell_search.h"
#include "srsran/phy/ue/ue_dl.h"
#include "srsran/phy/ue/ue_mib.h"
#include "srsran/phy/ue/ue_sync.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace cellscope::lte {

namespace {
uint64_t now_ms()
{
    using namespace std::chrono;
    return (uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

template <typename T, void (*FreeFn)(T*)>
struct SrsranDeleter {
    void operator()(T* ptr) const
    {
        if (ptr) {
            FreeFn(ptr);
            delete ptr;
        }
    }
};

    template <typename T, void (*FreeFn)(T*)>
    using SrsranHandle = std::unique_ptr<T, SrsranDeleter<T, FreeFn>>;

struct DecimFir {
    int                D = 1;
    std::vector<float> h;
    std::vector<cf_t>   hist;
    int                pos   = 0;
    int                phase = 0;

    void init(double in_rate, double out_rate)
    {
        D = (int)std::lround(in_rate / out_rate);
        if (D < 1) {
            D = 1;
        }
        if (D == 1) {
            h.clear();
            return;
        }
        const int    ntaps = 8 * D + 1;
        const int    M     = ntaps - 1;
        const double  fc   = 0.45 / D;
        h.resize(ntaps);
        double sum = 0.0;
        for (int i = 0; i < ntaps; ++i) {
            double x = i - M / 2.0;
            double s  = (std::fabs(x) < 1e-9) ? 2.0 * fc : std::sin(2.0 * M_PI * fc * x) / (M_PI * x);
            double w  = 0.54 - 0.46 * std::cos(2.0 * M_PI * i / M);
            h[i]      = (float)(s * w);
            sum      += h[i];
        }
        for (auto& v : h) {
            v /= (float)sum;
        }
        hist.assign(ntaps, cf_t{});
        pos   = 0;
        phase = 0;
    }

    void process(const cf_t* in, int n, std::vector<cf_t>& out)
    {
        const int ntaps = (int)h.size();
        for (int i = 0; i < n; ++i) {
            hist[pos] = in[i];
            pos       = (pos + 1) % ntaps;
            if (++phase >= D) {
                phase = 0;
                cf_t acc = 0;
                for (int j = 0; j < ntaps; ++j) {
                    int hi = pos - 1 - j;
                    hi %= ntaps;
                    if (hi < 0) {
                        hi += ntaps;
                    }
                    acc += h[j] * hist[hi];
                }
                out.push_back(acc);
            }
        }
    }
};

std::string operator_name(int mcc, int mnc)
{
    if (mcc == 310 || mcc == 311 || mcc == 312 || mcc == 313 || mcc == 316) {
        switch (mnc) {
            case 260: case 160: case 200: case 210: case 220: case 230:
            case 240: case 250: case 270: case 310: case 490: case 660: case 800:
                return "T-Mobile US";
            case 410: case 150: case 170: case 380: case 560: case 680: case 70:
                return "AT&T";
            case 4: case 10: case 12: case 13: case 480:
                return "Verizon";
            case 120:
                return "Sprint";
            default:
                return "USA";
        }
    }
    if (mcc == 234 || mcc == 235) return "UK";
    if (mcc == 262) return "Germany";
    if (mcc == 302) return "Canada";
    if (mcc == 505) return "Australia";
    return "";
}
} // namespace

struct LteEngine::Impl {
    EngineConfig cfg;

    mutable std::mutex      ring_mtx;
    std::condition_variable ring_cv;
    std::deque<cf_t>        ring;
    size_t                  ring_cap = 0;

    DecimFir              decim;
    std::vector<cf_t>     decim_out;
    srsran_resample_arb_t resampler{};
    bool                  resample_bypass = false;
    std::vector<cf_t>     resample_out;
    std::atomic<double>   target_rate{SRSRAN_CS_SAMP_FREQ};
    double                cfg_out_rate = 0.0;

    std::thread               worker;
    std::atomic<bool>         run{false};
    std::atomic<EngineState>  st{EngineState::Idle};

    mutable std::mutex status_mtx;
    std::string        status = "Idle";

    mutable std::mutex cell_mtx;
    bool               have_cell = false;
    CellInfo           cell_info;

    mutable std::mutex         ue_mtx;
    std::map<uint16_t, UeStat>  ues;
    std::map<uint16_t, uint64_t> prev_bytes;
    std::deque<HistPoint>       history_pts;
    std::deque<ConnEvent>       conn_events;
    uint64_t                    decode_start_ms = 0;
    uint64_t                    last_rate_ms    = 0;

    mutable std::mutex const_mtx;
    std::vector<float> const_pts;
    uint16_t           const_rnti = 0;

    std::vector<std::vector<cf_t>> sample_storage;
    std::vector<cf_t*>             sample_ptrs;

    static bool is_ue_rnti(uint16_t rnti)
    {
        return rnti != SRSRAN_SIRNTI && rnti != SRSRAN_PRNTI &&
               !(rnti >= SRSRAN_RARNTI_START && rnti <= SRSRAN_RARNTI_END) && rnti != 0;
    }

    static const char* rnti_channel(uint16_t rnti)
    {
        if (rnti == SRSRAN_SIRNTI) return "BCCH/SIB";
        if (rnti == SRSRAN_PRNTI) return "PCCH/Paging";
        if (rnti >= SRSRAN_RARNTI_START && rnti <= SRSRAN_RARNTI_END) return "RAR";
        return "DL-SCH";
    }

    void set_status(const std::string& s)
    {
        std::lock_guard<std::mutex> lk(status_mtx);
        status = s;
    }

    bool pop(cf_t* dst, uint32_t n)
    {
        std::unique_lock<std::mutex> lk(ring_mtx);
        ring_cv.wait(lk, [&] { return !run.load() || ring.size() >= n; });
        if (!run.load()) {
            return false;
        }
        for (uint32_t i = 0; i < n; ++i) {
            dst[i] = ring[i];
        }
        ring.erase(ring.begin(), ring.begin() + n);
        return true;
    }

    static int recv_cb(void* h, cf_t* data[SRSRAN_MAX_CHANNELS], uint32_t nsamples, srsran_timestamp_t* t)
    {
        auto* self = static_cast<Impl*>(h);
        if (t) {
            std::memset(t, 0, sizeof(*t));
        }
        if (!self->pop(data[0], nsamples)) {
            return SRSRAN_ERROR;
        }
        return (int)nsamples;
    }

    void configure_resampler(double out_rate)
    {
        decim.init(cfg.input_sample_rate_hz, out_rate);
        const double inter = cfg.input_sample_rate_hz / (double)decim.D;
        const double rate  = out_rate / inter;
        resample_bypass    = (rate > 0.999 && rate < 1.001);
        if (!resample_bypass) {
            srsran_resample_arb_init(&resampler, (float)rate, false);
        }
        ring_cap     = (size_t)(out_rate * 0.5);
        cfg_out_rate = out_rate;
    }

    void push_resampled(const cf_t* in, int n_in)
    {
        double tr = target_rate.load();
        if (tr != cfg_out_rate) {
            configure_resampler(tr);
            std::lock_guard<std::mutex> lk(ring_mtx);
            ring.clear();
        }

        const cf_t* stage = in;
        int         ns    = n_in;
        if (decim.D > 1) {
            decim_out.clear();
            decim.process(in, n_in, decim_out);
            stage = decim_out.data();
            ns    = (int)decim_out.size();
        }
        if (ns <= 0) {
            return;
        }

        const cf_t* src = stage;
        int         n   = ns;
        if (!resample_bypass) {
            // The ESP-SDR experiment can enter here below 1.92 MS/s, which is
            // interpolation rather than the usual SDR downsampling path.
            // Size for either direction so an undersampled source remains a
            // safe, testable input instead of overflowing the scratch buffer.
            const size_t needed = (size_t)std::ceil(ns * std::max(1.0, (double)resampler.rate)) + 64;
            if (resample_out.size() < needed) {
                resample_out.resize(needed);
            }
            n   = srsran_resample_arb_compute(&resampler, (cf_t*)stage, resample_out.data(), ns);
            src = resample_out.data();
        }

        std::lock_guard<std::mutex> lk(ring_mtx);
        for (int i = 0; i < n; ++i) {
            ring.push_back(src[i]);
        }
        if (ring.size() > ring_cap) {
            ring.erase(ring.begin(), ring.begin() + (ring.size() - ring_cap));
        }
        ring_cv.notify_one();
    }

    UeStat& ue_ref(uint16_t rnti, uint64_t ts)
    {
        auto it = ues.find(rnti);
        if (it != ues.end()) {
            return it->second;
        }
        UeStat u;
        u.rnti          = rnti;
        u.first_seen_ms = ts;
        u.last_seen_ms  = ts;
        auto res = ues.emplace(rnti, u);
        ConnEvent ev;
        ev.ts_ms = ts;
        ev.t_sec = decode_start_ms ? (ts - decode_start_ms) / 1000.0 : 0.0;
        ev.rnti  = rnti;
        conn_events.push_back(ev);
        while (conn_events.size() > 200) {
            conn_events.pop_front();
        }
        return res.first->second;
    }

    void track_common_rnti(uint16_t rnti, uint32_t bytes, int mcs, const std::string& summary)
    {
        const uint64_t ts = now_ms();
        if (is_ue_rnti(rnti)) {
            std::lock_guard<std::mutex> lk(ue_mtx);
            UeStat& u = ue_ref(rnti, ts);
            u.last_seen_ms = ts;
            u.dl_bytes += bytes;
            u.dl_msgs++;
            if (mcs >= 0) {
                u.last_mcs = mcs;
            }
        }

        TrafficMsg m;
        m.ts_ms     = ts;
        m.rnti      = rnti;
        m.direction = 0;
        m.channel   = rnti_channel(rnti);
        m.summary   = summary;
        m.len       = bytes;
        {
            std::lock_guard<std::mutex> tlk(traffic_mtx);
            traffic.push_back(std::move(m));
            while (traffic.size() > 1000) {
                traffic.pop_front();
            }
        }
    }

    static uint32_t dci_bytes(srsran_ue_dl_t* ue_dl, srsran_dl_sf_cfg_t* dl_sf, srsran_ue_dl_cfg_t* dl_cfg, srsran_dci_dl_t* dci)
    {
        srsran_pdsch_grant_t grant = {};
        if (srsran_ue_dl_dci_to_pdsch_grant(ue_dl, dl_sf, dl_cfg, dci, &grant)) {
            return 0;
        }
        uint32_t bytes = 0;
        for (int tb = 0; tb < SRSRAN_MAX_CODEWORDS; ++tb) {
            if (grant.tb[tb].enabled && grant.tb[tb].tbs > 0) {
                bytes += (uint32_t)(grant.tb[tb].tbs / 8);
            }
        }
        return bytes;
    }

    mutable std::mutex      traffic_mtx;
    std::deque<TrafficMsg>  traffic;

    void on_sib(const uint8_t* pdu, int len)
    {
        {
            std::lock_guard<std::mutex> lk(cell_mtx);
            if (cell_info.have_sib) {
                return;
            }
        }

        asn1::rrc::bcch_dl_sch_msg_s msg;
        asn1::cbit_ref bref(pdu, len);
        if (msg.unpack(bref) != asn1::SRSASN_SUCCESS) {
            return;
        }
        if (msg.msg.type().value != asn1::rrc::bcch_dl_sch_msg_type_c::types_opts::c1) {
            return;
        }
        if (msg.msg.c1().type().value != asn1::rrc::bcch_dl_sch_msg_type_c::c1_c_::types::sib_type1) {
            return;
        }

        auto& sib1 = msg.msg.c1().sib_type1();
        auto& cari = sib1.cell_access_related_info;

        int         mcc = -1, mnc = -1, mnc_dig = 0;
        std::string plmn;
        if (cari.plmn_id_list.size() > 0) {
            auto& p = cari.plmn_id_list[0].plmn_id;
            if (p.mcc_present) {
                mcc = p.mcc[0] * 100 + p.mcc[1] * 10 + p.mcc[2];
            }
            mnc_dig = (int)p.mnc.size();
            int m   = 0;
            for (uint32_t i = 0; i < p.mnc.size(); ++i) {
                m = m * 10 + p.mnc[i];
            }
            mnc = m;
            char buf[24];
            if (p.mcc_present) {
                std::snprintf(buf, sizeof(buf), "%03d-%0*d", mcc, mnc_dig, mnc);
            } else {
                std::snprintf(buf, sizeof(buf), "???-%0*d", mnc_dig, mnc);
            }
            plmn = buf;
        }

        uint32_t tac    = (uint32_t)cari.tac.to_number();
        uint64_t cid    = (uint64_t)cari.cell_id.to_number();
        bool     barred = (std::strcmp(cari.cell_barred.to_string(), "barred") == 0);
        int      band   = (int)sib1.freq_band_ind;
        std::string oper = operator_name(mcc, mnc);

        std::lock_guard<std::mutex> lk(cell_mtx);
        cell_info.have_sib = true;
        cell_info.plmn     = plmn;
        cell_info.oper     = oper;
        cell_info.tac      = tac;
        cell_info.cell_id  = cid;
        cell_info.band     = band;
        cell_info.barred   = barred;
        diagLog("SIB1: plmn=" + plmn + " oper=" + oper + " tac=" + std::to_string(tac) +
                " ecgi=" + std::to_string(cid) + " band=" + std::to_string(band));
    }

    static void sib_cb(void* user, const uint8_t* pdu, int len)
    {
        static_cast<Impl*>(user)->on_sib(pdu, len);
    }

    void update_rates()
    {
        std::lock_guard<std::mutex> lk(ue_mtx);
        const uint64_t now = now_ms();
        if (last_rate_ms == 0) {
            last_rate_ms = now;
        }
        double dt = (now - last_rate_ms) / 1000.0;
        if (dt < 0.2) {
            return;
        }
        double total_bps = 0.0;
        int    active    = 0;
        for (auto& kv : ues) {
            uint16_t rnti = kv.first;
            UeStat&  u    = kv.second;
            uint64_t prev  = prev_bytes.count(rnti) ? prev_bytes[rnti] : 0;
            double   inst  = (double)(u.dl_bytes - prev) * 8.0 / dt;
            const double alpha = 0.4;
            u.dl_bps = alpha * inst + (1.0 - alpha) * u.dl_bps;
            if (u.dl_bps < 1.0) {
                u.dl_bps = 0.0;
            }
            prev_bytes[rnti] = u.dl_bytes;
            total_bps += u.dl_bps;
            if (now - u.last_seen_ms < 3000) {
                active++;
            }
        }
        HistPoint hp;
        hp.t_sec      = decode_start_ms ? (now - decode_start_ms) / 1000.0 : 0.0;
        hp.dl_kbps    = total_bps / 1000.0;
        hp.active_ues = active;
        history_pts.push_back(hp);
        while (history_pts.size() > 600) {
            history_pts.pop_front();
        }
        last_rate_ms = now;
    }

    bool search_cell(CellInfo& out)
    {
        SrsranHandle<srsran_ue_cellsearch_t, srsran_ue_cellsearch_free> cs(new srsran_ue_cellsearch_t{});
        diagLog("search_cell start rx_antennas=" + std::to_string(cfg.nof_rx_antennas) +
                " center_mhz=" + std::to_string(cfg.center_freq_mhz));
        if (srsran_ue_cellsearch_init_multi(cs.get(), 8, recv_cb, cfg.nof_rx_antennas, this)) {
            set_status("cellsearch init failed");
            diagLog("search_cell init_multi failed");
            return false;
        }
        srsran_ue_cellsearch_set_nof_valid_frames(cs.get(), 4);

        srsran_ue_cellsearch_result_t found[3] = {};
        uint32_t                      max_nid2 = 0;
        int                           ret      = srsran_ue_cellsearch_scan(cs.get(), found, &max_nid2);
        bool ok = false;
        const float psr   = found[max_nid2].psr;
        const float peak  = found[max_nid2].peak;
        const float cfo   = found[max_nid2].cfo;
        if (ret > 0 && psr > 3.0f && peak > 0.02f) {
            out.pci      = (int)found[max_nid2].cell_id;
            out.peak     = peak;
            out.cfo_hz   = cfo;
            out.freq_mhz = cfg.center_freq_mhz;
            ok           = true;
        } else if (ret > 0) {
            diagLog("search_cell weak candidate ignored pci=" + std::to_string((int)found[max_nid2].cell_id) +
                    " psr=" + std::to_string(psr) +
                    " peak=" + std::to_string(peak) +
                    " cfo=" + std::to_string(cfo));
        }
        diagLog("search_cell ret=" + std::to_string(ret) +
                " max_nid2=" + std::to_string(max_nid2) +
                " psr=" + std::to_string(psr) +
                " peak=" + std::to_string(peak) +
                " cfo=" + std::to_string(cfo) +
                (ok ? " ok" : " failed"));
        return ok;
    }

    bool decode_mib(CellInfo& io)
    {
        srsran_cell_t cell = {};
        cell.id            = (uint32_t)io.pci;
        cell.cp            = SRSRAN_CP_NORM;
        cell.nof_ports     = 1;
        cell.nof_prb       = SRSRAN_UE_MIB_NOF_PRB;

        SrsranHandle<srsran_ue_mib_sync_t, srsran_ue_mib_sync_free> mib(new srsran_ue_mib_sync_t{});

        diagLog("decode_mib start pci=" + std::to_string(io.pci) +
                " rx_antennas=" + std::to_string(cfg.nof_rx_antennas));

        if (srsran_ue_mib_sync_init_multi(mib.get(), recv_cb, cfg.nof_rx_antennas, this)) {
            set_status("mib sync init failed");
            diagLog("decode_mib pci=" + std::to_string(io.pci) + " init_multi failed");
            return false;
        }

        bool cell_ok = (srsran_ue_mib_sync_set_cell(mib.get(), cell) == SRSRAN_SUCCESS);
        if (!cell_ok) {
            set_status("mib sync set_cell failed");
            diagLog("decode_mib pci=" + std::to_string(io.pci) + " set_cell failed");
        } else {
            mib->ue_sync.cfo_correct_enable_track = true;
        }

        uint8_t  bch[SRSRAN_BCH_PAYLOAD_LEN] = {};
        uint32_t nof_ports                   = 0;
        int      sfn_offset                  = 0;
        int      ret                         = SRSRAN_UE_MIB_NOTFOUND;

        if (cell_ok) {
            ret = srsran_ue_mib_sync_decode(mib.get(), 40, bch, &nof_ports, &sfn_offset);
        }

        diagLog("decode_mib pci=" + std::to_string(io.pci) +
                " ret=" + std::to_string(ret) +
                " nof_ports=" + std::to_string(nof_ports) +
                " sfn_offset=" + std::to_string(sfn_offset));
        if (ret == SRSRAN_UE_MIB_FOUND) {
            uint32_t sfn = 0;
            srsran_pbch_mib_unpack(bch, &cell, &sfn);
            io.nof_prb   = (int)cell.nof_prb;
            io.nof_ports = (int)nof_ports;
            io.sfn       = (int)sfn;
        }

        if (!cell_ok) {
            return false;
        }
        return ret == SRSRAN_UE_MIB_FOUND;
    }

    void decode_common_rars(srsran_ue_dl_t& ue_dl,
                            srsran_dl_sf_cfg_t& dl_sf,
                            srsran_ue_dl_cfg_t& dl_cfg)
    {
        srsran_dci_dl_t dci[SRSRAN_MAX_DCI_MSG] = {};
        for (uint16_t rnti = SRSRAN_RARNTI_START; rnti <= SRSRAN_RARNTI_END; ++rnti) {
            int ndci = srsran_ue_dl_find_dl_dci(&ue_dl, &dl_sf, &dl_cfg, rnti, dci);
            if (ndci <= 0) {
                continue;
            }
            for (int i = 0; i < ndci; ++i) {
                char info[256] = {};
                srsran_dci_dl_info(&dci[i], info, sizeof(info));
                track_common_rnti(rnti, dci_bytes(&ue_dl, &dl_sf, &dl_cfg, &dci[i]), -1, info);
            }
        }
    }

    void run_decode(CellInfo& ci)
    {
        srsran_cell_t cell = {};
        cell.id              = (uint32_t)ci.pci;
        cell.nof_prb         = (uint32_t)ci.nof_prb;
        cell.nof_ports       = ci.nof_ports > 0 ? (uint32_t)ci.nof_ports : 1;
        cell.cp              = SRSRAN_CP_NORM;
        cell.phich_length    = SRSRAN_PHICH_NORM;
        cell.phich_resources = SRSRAN_PHICH_R_1_6;
        cell.frame_type      = SRSRAN_FDD;

        const int srate = srsran_sampling_freq_hz(cell.nof_prb);
        if (srate <= 0) {
            set_status("decode: invalid PRB");
            return;
        }

        target_rate.store((double)srate);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        const uint32_t max_num_samples = 3 * SRSRAN_SF_LEN_PRB(cell.nof_prb);
        sample_storage.assign(cfg.nof_rx_antennas, {});
        sample_ptrs.resize(cfg.nof_rx_antennas);
        for (int i = 0; i < cfg.nof_rx_antennas; ++i) {
            sample_storage[i].resize(max_num_samples);
            sample_ptrs[i] = sample_storage[i].data();
        }

        SrsranHandle<srsran_ue_sync_t, srsran_ue_sync_free> ue_sync(new srsran_ue_sync_t{});
        if (srsran_ue_sync_init_multi(ue_sync.get(), cell.nof_prb, cell.id == 1000, recv_cb, cfg.nof_rx_antennas, this)) {
            set_status("decode: ue_sync init failed");
            return;
        }
        if (srsran_ue_sync_set_cell(ue_sync.get(), cell)) {
            set_status("decode: ue_sync set_cell failed");
            return;
        }
        ue_sync->cfo_correct_enable_track = true;

        SrsranHandle<srsran_ue_dl_t, srsran_ue_dl_free> ue_dl(new srsran_ue_dl_t{});
        if (srsran_ue_dl_init(ue_dl.get(), sample_ptrs.data(), cell.nof_prb, cfg.nof_rx_antennas)) {
            set_status("decode: ue_dl init failed");
            return;
        }
        if (srsran_ue_dl_set_cell(ue_dl.get(), cell)) {
            set_status("decode: ue_dl set_cell failed");
            return;
        }

        SrsranHandle<srsran_ue_mib_t, srsran_ue_mib_free> ue_mib(new srsran_ue_mib_t{});
        if (srsran_ue_mib_init(ue_mib.get(), sample_ptrs[0], cell.nof_prb) ||
            srsran_ue_mib_set_cell(ue_mib.get(), cell)) {
            set_status("decode: ue_mib init failed");
            return;
        }
        srsran_pbch_decode_reset(&ue_mib->pbch);

        srsran_ue_dl_cfg_t dl_cfg = {};
        dl_cfg.cfg.tm             = SRSRAN_TM1;
        dl_cfg.cfg.dci_common_ss  = true;
        dl_cfg.cfg.pdsch.use_tbs_index_alt = false;
        dl_cfg.snr_to_cqi_offset  = 0.0f;

        st.store(EngineState::Decoding);
        set_status("Decoding cell PCI " + std::to_string(cell.id) + " (" +
                   std::to_string(cell.nof_prb) + " PRB, " +
                   std::to_string((int)(srate / 1e6)) + " Msps)");
        diagLog("run_decode pci=" + std::to_string(cell.id) +
                " prb=" + std::to_string(cell.nof_prb) +
                " rx_antennas=" + std::to_string(cfg.nof_rx_antennas) +
                " sample_rate=" + std::to_string(srate));

        {
            std::lock_guard<std::mutex> lk(ue_mtx);
            decode_start_ms = now_ms();
            last_rate_ms    = decode_start_ms;
            history_pts.clear();
            conn_events.clear();
            prev_bytes.clear();
        }

        uint32_t sfn = (uint32_t)ci.sfn >= 0 ? (uint32_t)ci.sfn : 0;
        uint64_t sf_cnt = 0;
        uint64_t last_status_ms = now_ms();
        uint64_t last_raw_ms = last_status_ms;

        while (run.load()) {
            int ret = srsran_ue_sync_zerocopy(ue_sync.get(), sample_ptrs.data(), max_num_samples);
            if (ret < 0) {
                uint64_t now = now_ms();
                if (now - last_raw_ms > 1000) {
                    diagLog("Error receiving samples: srsran_ue_sync_zerocopy ret=" + std::to_string(ret) +
                            " pci=" + std::to_string(cell.id) +
                            " sf_cnt=" + std::to_string(sf_cnt));
                    last_raw_ms = now;
                }
                continue;
            }
            if (ret == 1) {
                uint32_t sf_idx = srsran_ue_sync_get_sfidx(ue_sync.get());
                if (sf_idx == 0) {
                    uint8_t  bch[SRSRAN_BCH_PAYLOAD_LEN] = {};
                    uint32_t nof_ports = 0;
                    int      sfn_offset = 0;
                    int n = srsran_ue_mib_decode(ue_mib.get(), bch, &nof_ports, &sfn_offset);
                    if (n == SRSRAN_UE_MIB_FOUND) {
                        srsran_pbch_mib_unpack(bch, &cell, &sfn);
                        sfn          = (sfn + sfn_offset) % 1024;
                        ci.sfn       = (int)sfn;
                        ci.nof_ports = (int)nof_ports;
                    }
                }

                srsran_dl_sf_cfg_t dl_sf = {};
                dl_sf.tti     = sfn * 10 + sf_idx;
                dl_sf.sf_type = SRSRAN_SF_NORM;

                srsran_ue_dl_set_mi_auto(ue_dl.get());
                if (srsran_ue_dl_decode_fft_estimate(ue_dl.get(), &dl_sf, &dl_cfg) == 0) {
                    srsran_dci_dl_t dci[SRSRAN_MAX_DCI_MSG] = {};
                    int ndci = srsran_ue_dl_find_dl_dci(ue_dl.get(), &dl_sf, &dl_cfg, SRSRAN_SIRNTI, dci);
                    for (int i = 0; i < ndci; ++i) {
                        char info[256] = {};
                        srsran_dci_dl_info(&dci[i], info, sizeof(info));
                        track_common_rnti(SRSRAN_SIRNTI, dci_bytes(ue_dl.get(), &dl_sf, &dl_cfg, &dci[i]), -1, info);
                    }
                    ndci = srsran_ue_dl_find_dl_dci(ue_dl.get(), &dl_sf, &dl_cfg, SRSRAN_PRNTI, dci);
                    for (int i = 0; i < ndci; ++i) {
                        char info[256] = {};
                        srsran_dci_dl_info(&dci[i], info, sizeof(info));
                        track_common_rnti(SRSRAN_PRNTI, dci_bytes(ue_dl.get(), &dl_sf, &dl_cfg, &dci[i]), -1, info);
                    }
                    decode_common_rars(*ue_dl, dl_sf, dl_cfg);
                    {
                        std::lock_guard<std::mutex> lk(cell_mtx);
                        cell_info.sfn = (int)sfn;
                    }
                }

                uint64_t now = now_ms();
                if (now - last_raw_ms > 1500) {
                    diagLog("decode tick pci=" + std::to_string(cell.id) +
                            " sfn=" + std::to_string(sfn) +
                            " sf_idx=" + std::to_string(sf_idx) +
                            " sf_cnt=" + std::to_string(sf_cnt));
                    last_raw_ms = now;
                }

                if (sf_idx == 9) {
                    sfn = (sfn + 1) % 1024;
                }
                sf_cnt++;
            }

            uint64_t now = now_ms();
            if (now - last_status_ms > 1000) {
                update_rates();
                size_t nue;
                {
                    std::lock_guard<std::mutex> lk(ue_mtx);
                    nue = ues.size();
                }
                set_status("Decoding PCI " + std::to_string(cell.id) + ": " +
                           std::to_string(nue) + " channels, " + std::to_string(sf_cnt) + " subframes");
                last_status_ms = now;
            }
        }
    }

    void run_worker()
    {
        st.store(EngineState::Searching);
        CellInfo info;
        int attempts = 0;
        while (run.load()) {
            if (!search_cell(info)) {
                set_status("Searching for cell... (attempt " + std::to_string(++attempts) + ")");
                diagLog("Error searching cell attempt=" + std::to_string(attempts));
                continue;
            }
            set_status("Cell PCI " + std::to_string(info.pci) + " found, decoding MIB...");
            if (decode_mib(info)) {
                {
                    std::lock_guard<std::mutex> lk(cell_mtx);
                    cell_info = info;
                    have_cell = true;
                }
                st.store(EngineState::CellFound);
                set_status("Cell PCI " + std::to_string(info.pci) + ", " +
                           std::to_string(info.nof_prb) + " PRB, " +
                           std::to_string(info.nof_ports) + " ports");
                break;
            }
            set_status("MIB decode failed, re-searching...");
            diagLog("MIB decode failed pci=" + std::to_string(info.pci));
        }

        if (run.load() && st.load() == EngineState::CellFound) {
            run_decode(info);
        }
    }
};

LteEngine::LteEngine() : impl_(std::make_unique<Impl>()) {}

LteEngine::~LteEngine()
{
    stop();
}

void LteEngine::start(const EngineConfig& cfg)
{
#if defined(__EMSCRIPTEN__)
    (void)cfg;
    stop();
    impl_->run.store(false);
    impl_->st.store(EngineState::Idle);
    impl_->set_status("LTE decode disabled in browser build");
    return;
#else
    stop();
    diagInstallCrashHandler();
    diagLog("LteEngine::start input_rate=" + std::to_string(cfg.input_sample_rate_hz) +
            " center_mhz=" + std::to_string(cfg.center_freq_mhz));
    impl_->cfg = cfg;

    const double out_rate = SRSRAN_CS_SAMP_FREQ;
    impl_->target_rate.store(out_rate);
    impl_->configure_resampler(out_rate);
    {
        std::lock_guard<std::mutex> lk(impl_->ring_mtx);
        impl_->ring.clear();
    }
    impl_->have_cell = false;
    impl_->run.store(true);
    impl_->st.store(EngineState::Searching);
    {
        char buf[160];
        std::snprintf(buf,
                      sizeof(buf),
                      "Input %.3f Msps -> resample to 1.92 Msps (stage /%d); searching...",
                      cfg.input_sample_rate_hz / 1e6,
                      impl_->decim.D);
        impl_->set_status(buf);
    }
    impl_->worker = std::thread([this] { impl_->run_worker(); });
#endif
}

void LteEngine::stop()
{
#if defined(__EMSCRIPTEN__)
    impl_->run.store(false);
    impl_->st.store(EngineState::Idle);
    impl_->set_status("Idle");
    return;
#else
    if (!impl_->run.exchange(false)) {
        if (impl_->worker.joinable()) {
            impl_->worker.join();
        }
        return;
    }
    impl_->ring_cv.notify_all();
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }
    impl_->st.store(EngineState::Idle);
    impl_->set_status("Idle");
#endif
}

bool LteEngine::running() const
{
    return impl_->run.load();
}

void LteEngine::feed(const std::complex<float>* iq, std::size_t n)
{
    if (!impl_->run.load() || n == 0) {
        return;
    }
    impl_->push_resampled(reinterpret_cast<const cf_t*>(iq), (int)n);
}

EngineState LteEngine::state() const
{
    return impl_->st.load();
}

std::string LteEngine::statusText() const
{
    std::lock_guard<std::mutex> lk(impl_->status_mtx);
    return impl_->status;
}

bool LteEngine::cell(CellInfo& out) const
{
    std::lock_guard<std::mutex> lk(impl_->cell_mtx);
    if (!impl_->have_cell) {
        return false;
    }
    out = impl_->cell_info;
    return true;
}

std::vector<UeStat> LteEngine::snapshotUes() const
{
    std::lock_guard<std::mutex> lk(impl_->ue_mtx);
    std::vector<UeStat> v;
    v.reserve(impl_->ues.size());
    for (auto& kv : impl_->ues) {
        v.push_back(kv.second);
    }
    return v;
}

std::vector<TrafficMsg> LteEngine::drainTraffic()
{
    std::lock_guard<std::mutex> lk(impl_->traffic_mtx);
    std::vector<TrafficMsg> v(impl_->traffic.begin(), impl_->traffic.end());
    impl_->traffic.clear();
    return v;
}

std::vector<HistPoint> LteEngine::history() const
{
    std::lock_guard<std::mutex> lk(impl_->ue_mtx);
    return std::vector<HistPoint>(impl_->history_pts.begin(), impl_->history_pts.end());
}

std::vector<ConnEvent> LteEngine::connections() const
{
    std::lock_guard<std::mutex> lk(impl_->ue_mtx);
    return std::vector<ConnEvent>(impl_->conn_events.begin(), impl_->conn_events.end());
}

void LteEngine::setWatched(const std::vector<uint16_t>& rntis)
{
    std::lock_guard<std::mutex> lk(impl_->ue_mtx);
    std::set<uint16_t> next(rntis.begin(), rntis.end());
    for (auto it = impl_->ues.begin(); it != impl_->ues.end();) {
        if (!next.count(it->first)) {
            it = impl_->ues.erase(it);
        } else {
            ++it;
        }
    }
}

std::vector<HistPoint> LteEngine::ueHistory(uint16_t) const
{
    return {};
}

std::vector<float> LteEngine::constellation(uint16_t& outRnti) const
{
    std::lock_guard<std::mutex> lk(impl_->const_mtx);
    outRnti = impl_->const_rnti;
    return impl_->const_pts;
}

std::vector<CallEvent> LteEngine::callLog() const
{
    return {};
}

} // namespace cellscope::lte
