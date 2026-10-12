// Matter node (esp-matter): the batteries for Apple Home. See matter_svc.h for what is exposed.
//
// Endpoints (ids are assigned in creation order, after the root node on 0):
//   Battery 1, Battery 2  temperature sensor + Power Source (Battery, Rechargeable): the tile shows the battery's
//                         temperature; the accessory details show its charge, charging state and low battery
//   Battery bank          metered plug (its switch is a stand-in, always on): power drawn from the bank, its
//                         voltage and current, and the energy drawn (kWh) for the Home app's Energy view
//
// Pairing is on-network only. The panel is already on Wi-Fi, so Matter doesn't need BLE, and leaving CHIPoBLE off
// keeps NimBLE for VanDeck's own scanning. The setup passcode and discriminator are random per panel (vd_mt in NVS);
// a well-known test code would let anyone on a shared campsite network pair the panel before you do.
#include "matter_svc.h"
#include <atomic>
#include <string.h>
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <esp_matter.h>
#include <esp_matter_core.h>
#include <esp_matter_providers.h>
#include <app/server/Server.h>
#include <app/reporting/reporting.h>
#include <app/clusters/electrical-power-measurement-server/ElectricalPowerMeasurementDelegate.h>
#include <app/clusters/electrical-energy-measurement-server/CodegenIntegration.h>
#include <app/clusters/temperature-measurement-server/TemperatureMeasurementCluster.h>
#include <esp_matter_data_model_provider.h>
#include "esp_timer.h"
#include <crypto/CHIPCryptoPAL.h>
#include <platform/CommissionableDataProvider.h>
#include <platform/CHIPDeviceLayer.h>
#include <setup_payload/OnboardingCodesUtil.h>
#include <setup_payload/SetupPayload.h>

extern "C" {
#include "app/model.h"
}

using namespace esp_matter;
using namespace chip::app::Clusters;

static const char *TAG = "matter";

static std::atomic<vd_mt_state_t> s_state{MT_OFF};
static std::atomic<uint8_t> s_fabrics{0};
static char s_qr[64], s_manual[16];
static bool s_skipped;   // the last start didn't survive a minute: not tried again until 'matter retry'

// Crash guard: "try" is set in NVS before Matter starts and cleared once it has run for a minute. Still set after a
// crash means that start brought the panel down, so it is skipped and the rest of the panel keeps working.
static void guard_set(uint8_t v)
{
    nvs_handle_t h;
    if (nvs_open("vd_mt", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "try", v);
    nvs_commit(h);
    nvs_close(h);
}

// Only a crash counts: in a van the power often goes within a minute of starting (ignition), and a restart from
// the console or an update isn't Matter's fault either.
static bool guard_tripped(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open("vd_mt", NVS_READONLY, &h) == ESP_OK) { nvs_get_u8(h, "try", &v); nvs_close(h); }
    esp_reset_reason_t r = esp_reset_reason();
    return v && (r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT);
}

// --- Setup code: generated once, kept in NVS so the code on the screen stays the same across restarts and resets

class VdCommissionable : public chip::DeviceLayer::CommissionableDataProvider {
public:
    bool init()
    {
        nvs_handle_t h;
        if (nvs_open("vd_mt", NVS_READWRITE, &h) != ESP_OK) return false;
        size_t n = sizeof m_salt;
        if (nvs_get_u32(h, "pin", &m_pin) != ESP_OK || nvs_get_u16(h, "disc", &m_disc) != ESP_OK ||
            nvs_get_blob(h, "salt", m_salt, &n) != ESP_OK || n != sizeof m_salt) {
            do m_pin = 1 + esp_random() % 99999998; while (!chip::SetupPayload::IsValidSetupPIN(m_pin));
            m_disc = esp_random() & 0xFFF;
            esp_fill_random(m_salt, sizeof m_salt);
            nvs_set_u32(h, "pin", m_pin);
            nvs_set_u16(h, "disc", m_disc);
            nvs_set_blob(h, "salt", m_salt, sizeof m_salt);
            nvs_commit(h);
            ESP_LOGI(TAG, "new setup code generated");
        }
        nvs_close(h);
        return true;
    }

    CHIP_ERROR GetSetupDiscriminator(uint16_t &d) override { d = m_disc; return CHIP_NO_ERROR; }
    CHIP_ERROR SetSetupDiscriminator(uint16_t) override { return CHIP_ERROR_NOT_IMPLEMENTED; }
    CHIP_ERROR GetSpake2pIterationCount(uint32_t &c) override { c = kIterations; return CHIP_NO_ERROR; }
    CHIP_ERROR GetSpake2pSalt(chip::MutableByteSpan &buf) override { return chip::CopySpanToMutableSpan(chip::ByteSpan(m_salt), buf); }
    // Computed on first use, on the CHIP thread: the verifier (PBKDF2 + P-256) needs CHIP's crypto set up, and
    // computing it before esp_matter::start() panicked on the panel.
    CHIP_ERROR GetSpake2pVerifier(chip::MutableByteSpan &buf, size_t &len) override
    {
        if (!m_verifier_len) {
            chip::Crypto::Spake2pVerifier v;
            ReturnErrorOnFailure(v.Generate(kIterations, chip::ByteSpan(m_salt), m_pin));
            chip::MutableByteSpan out(m_verifier);
            ReturnErrorOnFailure(v.Serialize(out));
            m_verifier_len = out.size();
        }
        len = m_verifier_len;
        return chip::CopySpanToMutableSpan(chip::ByteSpan(m_verifier, m_verifier_len), buf);
    }
    CHIP_ERROR GetSetupPasscode(uint32_t &p) override { p = m_pin; return CHIP_NO_ERROR; }
    CHIP_ERROR SetSetupPasscode(uint32_t) override { return CHIP_ERROR_NOT_IMPLEMENTED; }

private:
    static constexpr uint32_t kIterations = 1000;
    uint32_t m_pin = 0;
    uint16_t m_disc = 0;
    uint8_t m_salt[32];
    chip::Crypto::Spake2pVerifierSerialized m_verifier;
    size_t m_verifier_len = 0;
};

static VdCommissionable s_commissionable;

// --- Bank power: the Electrical Power Measurement cluster reads its values through a delegate

namespace EPM = ElectricalPowerMeasurement;
namespace EEM = ElectricalEnergyMeasurement;
using MeasType = detail::MeasurementTypeEnum;
using AccRange = detail::Structs::MeasurementAccuracyRangeStruct::Type;

class BankPower : public EPM::Delegate {
public:
    // mW, mV, mA; null while no battery is reporting
    chip::app::DataModel::Nullable<int64_t> power, volts, amps;

    EPM::PowerModeEnum GetPowerMode() override { return EPM::PowerModeEnum::kDc; }
    uint8_t GetNumberOfMeasurementTypes() override { return 3; }

    CHIP_ERROR StartAccuracyRead() override { return CHIP_NO_ERROR; }
    CHIP_ERROR GetAccuracyByIndex(uint8_t i, EPM::Structs::MeasurementAccuracyStruct::Type &out) override
    {
        if (i >= 3) return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
        static const MeasType T[] = { MeasType::kActivePower, MeasType::kVoltage, MeasType::kActiveCurrent };
        static const int64_t MAX[] = { 10000000, 30000, 500000 };   // 10 kW, 30 V, 500 A
        out.measurementType = T[i];
        out.measured = true;
        out.minMeasuredValue = i == 1 ? 0 : -MAX[i];
        out.maxMeasuredValue = MAX[i];
        m_range[i].rangeMin = out.minMeasuredValue;
        m_range[i].rangeMax = out.maxMeasuredValue;
        m_range[i].percentMax.SetValue(200);   // 2 %: what the BMS reports, not a calibrated meter
        out.accuracyRanges = chip::app::DataModel::List<const AccRange>(&m_range[i], 1);
        return CHIP_NO_ERROR;
    }
    CHIP_ERROR EndAccuracyRead() override { return CHIP_NO_ERROR; }

    CHIP_ERROR StartRangesRead() override { return CHIP_NO_ERROR; }
    CHIP_ERROR GetRangeByIndex(uint8_t, EPM::Structs::MeasurementRangeStruct::Type &) override { return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED; }
    CHIP_ERROR EndRangesRead() override { return CHIP_NO_ERROR; }
    CHIP_ERROR StartHarmonicCurrentsRead() override { return CHIP_NO_ERROR; }
    CHIP_ERROR GetHarmonicCurrentsByIndex(uint8_t, EPM::Structs::HarmonicMeasurementStruct::Type &) override { return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED; }
    CHIP_ERROR EndHarmonicCurrentsRead() override { return CHIP_NO_ERROR; }
    CHIP_ERROR StartHarmonicPhasesRead() override { return CHIP_NO_ERROR; }
    CHIP_ERROR GetHarmonicPhasesByIndex(uint8_t, EPM::Structs::HarmonicMeasurementStruct::Type &) override { return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED; }
    CHIP_ERROR EndHarmonicPhasesRead() override { return CHIP_NO_ERROR; }

    chip::app::DataModel::Nullable<int64_t> GetVoltage() override { return volts; }
    chip::app::DataModel::Nullable<int64_t> GetActiveCurrent() override { return amps; }
    chip::app::DataModel::Nullable<int64_t> GetActivePower() override { return power; }
    chip::app::DataModel::Nullable<int64_t> GetReactiveCurrent() override { return {}; }
    chip::app::DataModel::Nullable<int64_t> GetApparentCurrent() override { return {}; }
    chip::app::DataModel::Nullable<int64_t> GetReactivePower() override { return {}; }
    chip::app::DataModel::Nullable<int64_t> GetApparentPower() override { return {}; }
    chip::app::DataModel::Nullable<int64_t> GetRMSVoltage() override { return {}; }
    chip::app::DataModel::Nullable<int64_t> GetRMSCurrent() override { return {}; }
    chip::app::DataModel::Nullable<int64_t> GetRMSPower() override { return {}; }
    chip::app::DataModel::Nullable<int64_t> GetFrequency() override { return {}; }
    chip::app::DataModel::Nullable<int64_t> GetPowerFactor() override { return {}; }
    chip::app::DataModel::Nullable<int64_t> GetNeutralCurrent() override { return {}; }

private:
    AccRange m_range[3];
};

static BankPower s_bank;
static uint16_t s_ep_batt[2], s_ep_bank;

// --- Data model

static esp_err_t on_attr(attribute::callback_type_t, uint16_t, uint32_t, uint32_t, esp_matter_attr_val_t *, void *) { return ESP_OK; }
static esp_err_t on_identify(identification::callback_type_t, uint16_t, uint8_t, uint8_t, void *) { return ESP_OK; }

static uint16_t add_battery(node_t *node, int i)
{
    endpoint::temperature_sensor::config_t tc;
    tc.temperature_measurement.measured_value = nullable<int16_t>();
    tc.temperature_measurement.min_measured_value = nullable<int16_t>(-4000);
    tc.temperature_measurement.max_measured_value = nullable<int16_t>(8000);
    endpoint_t *ep = endpoint::temperature_sensor::create(node, &tc, ENDPOINT_FLAG_NONE, nullptr);

    endpoint::power_source::config_t pc;
    pc.power_source.status = (uint8_t)PowerSource::PowerSourceStatusEnum::kUnavailable;
    pc.power_source.order = (uint8_t)i;
    snprintf(pc.power_source.description, sizeof pc.power_source.description, "Battery %d", i + 1);
    pc.power_source.feature_flags = cluster::power_source::feature::battery::get_id() | cluster::power_source::feature::rechargeable::get_id();
    pc.power_source.features.battery.bat_replaceability = (uint8_t)PowerSource::BatReplaceabilityEnum::kUserReplaceable;
    pc.power_source.features.rechargeable.bat_functional_while_charging = true;
    endpoint::power_source::add(ep, &pc);

    cluster_t *ps = cluster::get(ep, PowerSource::Id);
    cluster::power_source::attribute::create_bat_percent_remaining(ps, nullable<uint8_t>(), nullable<uint8_t>(0), nullable<uint8_t>(200));
    cluster::power_source::attribute::create_bat_voltage(ps, nullable<uint32_t>(), nullable<uint32_t>(0), nullable<uint32_t>(30000));
    cluster::power_source::attribute::create_bat_present(ps, false);
    return endpoint::get_id(ep);
}

// A metered plug: the shape Apple Home's Energy view lists (a bare Electrical Sensor endpoint doesn't show up at
// all). The switch is a stand-in: it always reports on, and push_bank() turns it back on if someone switches it off.
static uint16_t add_bank(node_t *node)
{
    endpoint::on_off_plug_in_unit::config_t pc;
    pc.on_off.on_off = true;
    endpoint_t *ep = endpoint::on_off_plug_in_unit::create(node, &pc, ENDPOINT_FLAG_NONE, nullptr);

    endpoint::electrical_sensor::config_t ec;
    // Power Topology needs exactly one topology; the bank's figures cover the whole node (esp-matter asserts
    // without one).
    ec.power_topology.feature_flags = cluster::power_topology::feature::node_topology::get_id();
    cluster::electrical_power_measurement::config_t &pm = ec.with_electrical_power_measurement();
    pm.delegate = &s_bank;
    pm.feature_flags = cluster::electrical_power_measurement::feature::direct_current::get_id();
    cluster::electrical_energy_measurement::config_t &em = ec.with_electrical_energy_measurement();
    em.feature_flags = cluster::electrical_energy_measurement::feature::imported_energy::get_id() |
                       cluster::electrical_energy_measurement::feature::cumulative_energy::get_id();
    endpoint::electrical_sensor::add(ep, &ec);

    cluster_t *c = cluster::get(ep, EPM::Id);
    cluster::electrical_power_measurement::attribute::create_voltage(c, nullable<int64_t>());
    cluster::electrical_power_measurement::attribute::create_active_current(c, nullable<int64_t>());
    return endpoint::get_id(ep);
}

// --- Keeping the attributes current

static void set(uint16_t ep, uint32_t cl, uint32_t at, esp_matter_attr_val_t v)
{
    esp_err_t e = attribute::update(ep, cl, at, &v);
    if (e != ESP_OK) ESP_LOGW(TAG, "update %u/0x%lx/0x%lx: %s", ep, (unsigned long)cl, (unsigned long)at, esp_err_to_name(e));
}

// Temperature Measurement is one of CHIP's code-driven clusters: it keeps its own value, and attribute::update()
// only reaches esp-matter's copy, which nobody reads (controllers saw null). Set it on the cluster itself.
static void set_temperature(uint16_t ep, chip::app::DataModel::Nullable<int16_t> v)
{
    lock::ScopedChipStackLock lock(portMAX_DELAY);
    chip::app::ServerClusterInterface *c =
        data_model::provider::get_instance().registry().Get(chip::app::ConcreteClusterPath(ep, TemperatureMeasurement::Id));
    if (!c) { ESP_LOGW(TAG, "no temperature cluster on endpoint %u", ep); return; }
    if (static_cast<TemperatureMeasurementCluster *>(c)->SetMeasuredValue(v) != CHIP_NO_ERROR)
        ESP_LOGW(TAG, "temperature on endpoint %u not set", ep);
}

static void push_battery(int i, const vd_batt_t *b)
{
    uint16_t ep = s_ep_batt[i];
    bool ok = b->valid && !b->stale;
    namespace PA = PowerSource::Attributes;
    set_temperature(ep, ok && !isnan(b->temp_c) ? chip::app::DataModel::Nullable<int16_t>((int16_t)lroundf(b->temp_c * 100))
                                                : chip::app::DataModel::Nullable<int16_t>());
    set(ep, PowerSource::Id, PA::Status::Id, esp_matter_enum8((uint8_t)(ok ? PowerSource::PowerSourceStatusEnum::kActive
                                                                          : PowerSource::PowerSourceStatusEnum::kUnavailable)));
    set(ep, PowerSource::Id, PA::BatPresent::Id, esp_matter_bool(b->valid));
    set(ep, PowerSource::Id, PA::BatPercentRemaining::Id,
        esp_matter_nullable_uint8(ok ? nullable<uint8_t>((uint8_t)lroundf(fminf(fmaxf(b->soc, 0), 100) * 2)) : nullable<uint8_t>()));
    set(ep, PowerSource::Id, PA::BatVoltage::Id,
        esp_matter_nullable_uint32(ok ? nullable<uint32_t>((uint32_t)lroundf(b->volts * 100) * 10) : nullable<uint32_t>()));   // 10 mV steps
    if (!ok) return;
    PowerSource::BatChargeLevelEnum lvl = b->soc <= 10 ? PowerSource::BatChargeLevelEnum::kCritical
                                        : b->soc <= 20 ? PowerSource::BatChargeLevelEnum::kWarning : PowerSource::BatChargeLevelEnum::kOk;
    set(ep, PowerSource::Id, PA::BatChargeLevel::Id, esp_matter_enum8((uint8_t)lvl));
    PowerSource::BatChargeStateEnum cs = b->amps > 0.3f ? PowerSource::BatChargeStateEnum::kIsCharging
                                       : b->soc >= 99.5f ? PowerSource::BatChargeStateEnum::kIsAtFullCharge
                                       : PowerSource::BatChargeStateEnum::kIsNotCharging;
    set(ep, PowerSource::Id, PA::BatChargeState::Id, esp_matter_enum8((uint8_t)cs));
}

// Energy drawn from the bank (mWh), kept in NVS every 50 Wh so the Home app's total doesn't restart from zero.
static int64_t s_energy_mwh, s_energy_saved = -1, s_energy_at_us;

static void energy_load(void)
{
    nvs_handle_t h;
    if (nvs_open("vd_mt", NVS_READONLY, &h) == ESP_OK) { nvs_get_i64(h, "e_mwh", &s_energy_mwh); nvs_close(h); }
    s_energy_saved = s_energy_mwh;
}

static void energy_save(void)
{
    nvs_handle_t h;
    if (nvs_open("vd_mt", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i64(h, "e_mwh", s_energy_mwh);
    nvs_commit(h);
    nvs_close(h);
    s_energy_saved = s_energy_mwh;
}

// The Energy view is about consumption, so the plug reports what the bank supplies: power drawn from it (0 while
// it charges), its voltage, and the discharge current.
static void push_bank(const vd_model_t *m)
{
    int n = 0;
    float w = 0, v = 0, a = 0;
    for (int i = 0; i < 2; i++) {
        const vd_batt_t *b = &m->batt[i];
        if (!b->valid || b->stale) continue;
        n++; w += b->volts * b->amps; v += b->volts; a += b->amps;
    }
    float out_w = n && w < 0 ? -w : 0, out_a = n && a < 0 ? -a : 0;

    int64_t now = esp_timer_get_time();
    if (s_energy_at_us && n) s_energy_mwh += (int64_t)llroundf(out_w * (float)(now - s_energy_at_us) / 3.6e6f);   // W * us -> mWh
    s_energy_at_us = now;
    if (s_energy_mwh - s_energy_saved >= 50000) energy_save();

    set(s_ep_bank, OnOff::Id, OnOff::Attributes::OnOff::Id, esp_matter_bool(true));

    lock::ScopedChipStackLock lock(portMAX_DELAY);
    using chip::app::DataModel::Nullable;
    Nullable<int64_t> p = n ? Nullable<int64_t>((int64_t)lroundf(out_w * 1000)) : Nullable<int64_t>();
    Nullable<int64_t> vv = n ? Nullable<int64_t>((int64_t)lroundf(v / n * 1000)) : Nullable<int64_t>();
    Nullable<int64_t> aa = n ? Nullable<int64_t>((int64_t)lroundf(out_a * 1000)) : Nullable<int64_t>();
    // Report only real changes: 1 W, 50 mV, 100 mA. The controller sees every report.
    auto moved = [](const Nullable<int64_t> &o, const Nullable<int64_t> &nw, int64_t step) {
        return o.IsNull() != nw.IsNull() || (!o.IsNull() && llabs(o.Value() - nw.Value()) >= step);
    };
    if (moved(s_bank.power, p, 1000)) { s_bank.power = p; MatterReportingAttributeChangeCallback(s_ep_bank, EPM::Id, EPM::Attributes::ActivePower::Id); }
    if (moved(s_bank.volts, vv, 50)) { s_bank.volts = vv; MatterReportingAttributeChangeCallback(s_ep_bank, EPM::Id, EPM::Attributes::Voltage::Id); }
    if (moved(s_bank.amps, aa, 100)) { s_bank.amps = aa; MatterReportingAttributeChangeCallback(s_ep_bank, EPM::Id, EPM::Attributes::ActiveCurrent::Id); }

    static int64_t s_reported = -1;
    if (s_energy_mwh - s_reported >= 1000 || s_reported < 0) {   // every Wh
        s_reported = s_energy_mwh;
        EEM::Structs::EnergyMeasurementStruct::Type e;
        e.energy = s_energy_mwh;
        EEM::NotifyCumulativeEnergyMeasured(s_ep_bank, Nullable<EEM::Structs::EnergyMeasurementStruct::Type>(e), Nullable<EEM::Structs::EnergyMeasurementStruct::Type>());
    }
}

static void update_task(void *)
{
    static vd_model_t m;   // large (Starlink history): not on this task's stack
    for (int n = 0;; n++) {
        if (n == 6) guard_set(0);   // a minute up: this start is good
        if (n % 6 == 0) {   // once a minute, at debug level (log matter debug): what controllers are being served
            esp_matter_attr_val_t t1 = {}, p1 = {};
            attribute::get_val(attribute::get(s_ep_batt[0], TemperatureMeasurement::Id, TemperatureMeasurement::Attributes::MeasuredValue::Id), &t1);
            attribute::get_val(attribute::get(s_ep_batt[0], PowerSource::Id, PowerSource::Attributes::BatPercentRemaining::Id), &p1);
            ESP_LOGD(TAG, "served: battery 1 %d (0.01 C), %u (half %%); bank %lld mW, %lld mWh",
                     t1.val.i16, p1.val.u8, s_bank.power.IsNull() ? -1LL : (long long)s_bank.power.Value(), (long long)s_energy_mwh);
        }
        model_snapshot(&m);
        for (int i = 0; i < 2; i++) push_battery(i, &m.batt[i]);
        push_bank(&m);
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

// --- Lifecycle

static void count_fabrics(void)
{
    uint8_t n = chip::Server::GetInstance().GetFabricTable().FabricCount();
    s_fabrics = n;
    s_state = n ? MT_PAIRED : MT_UNPAIRED;
}

static void on_event(const chip::DeviceLayer::ChipDeviceEvent *e, intptr_t)
{
    using namespace chip::DeviceLayer;
    switch (e->Type) {
    case DeviceEventType::kServerReady:
    case DeviceEventType::kCommissioningComplete:
    case DeviceEventType::kFabricRemoved:
    case DeviceEventType::kFabricCommitted:
        count_fabrics();
        if (e->Type == DeviceEventType::kCommissioningComplete) ESP_LOGI(TAG, "paired with a controller");
        // Last controller gone: reopen pairing so the panel can be added again without a reset.
        if (e->Type == DeviceEventType::kFabricRemoved && s_fabrics == 0) {
            if (chip::Server::GetInstance().GetCommissioningWindowManager().OpenBasicCommissioningWindow() != CHIP_NO_ERROR)
                ESP_LOGW(TAG, "couldn't reopen pairing");
        }
        break;
    default:
        break;
    }
}

// Steps are logged with the task's stack headroom: the first start on the panel panicked with nothing logged.
#define STEP(what) ESP_LOGI(TAG, "%s (stack left %u)", what, (unsigned)uxTaskGetStackHighWaterMark(NULL))

static void start_task(void *)
{
    STEP("setup code");
    if (!s_commissionable.init()) { ESP_LOGE(TAG, "setup code"); s_state = MT_ERROR; vTaskDelete(NULL); }
    set_custom_commissionable_data_provider(&s_commissionable);

    STEP("data model");
    energy_load();
    node::config_t nc;
    node_t *node = node::create(&nc, on_attr, on_identify);
    if (!node) { s_state = MT_ERROR; vTaskDelete(NULL); }
    for (int i = 0; i < 2; i++) s_ep_batt[i] = add_battery(node, i);
    s_ep_bank = add_bank(node);

    STEP("stack");
    if (esp_matter::start(on_event) != ESP_OK) { ESP_LOGE(TAG, "start failed"); s_state = MT_ERROR; vTaskDelete(NULL); }
    {
        lock::ScopedChipStackLock lock(portMAX_DELAY);
        // wifi_svc joins and rejoins networks; CHIP only uses the link.
        if (chip::DeviceLayer::ConnectivityMgr().SetWiFiStationMode(chip::DeviceLayer::ConnectivityManager::kWiFiStationMode_ApplicationControlled) != CHIP_NO_ERROR)
            ESP_LOGW(TAG, "Wi-Fi station mode");
        chip::RendezvousInformationFlags rf(chip::RendezvousInformationFlag::kOnNetwork);
        chip::MutableCharSpan qr(s_qr, sizeof s_qr - 1), mc(s_manual, sizeof s_manual - 1);
        if (GetQRCode(qr, rf) == CHIP_NO_ERROR) s_qr[qr.size()] = 0;
        if (GetManualPairingCode(mc, rf) == CHIP_NO_ERROR) s_manual[mc.size()] = 0;
        count_fabrics();
    }
    STEP("started");
    ESP_LOGI(TAG, "up: battery endpoints %u, %u; bank %u; %u controller(s)", s_ep_batt[0], s_ep_batt[1], s_ep_bank, s_fabrics.load());
    xTaskCreate(update_task, "matter_upd", 4096, nullptr, 3, nullptr);
    vTaskDelete(NULL);
}

extern "C" void matter_svc_init(void)
{
#if CONFIG_VD_MATTER
    if (guard_tripped()) {
        ESP_LOGE(TAG, "skipped: the last start didn't survive a minute ('matter retry' to try again)");
        s_skipped = true;
        s_state = MT_ERROR;
        return;
    }
    guard_set(1);
    s_state = MT_STARTING;
    esp_log_level_set("esp_matter_attribute", ESP_LOG_WARN);   // it logs every attribute write at info
    // Its own task: the SPAKE2+ verifier (PBKDF2 + P-256) and esp_matter::start() need more stack than start-up
    // has to spare. Internal RAM, since CHIP writes NVS from here (flash ops can't run on a PSRAM stack).
    if (xTaskCreate(start_task, "matter_start", 16384, nullptr, 3, nullptr) != pdPASS) { ESP_LOGE(TAG, "no memory for start task"); s_state = MT_ERROR; }
#endif
}

extern "C" vd_mt_state_t matter_svc_state(void) { return s_state; }
extern "C" bool matter_svc_skipped(void) { return s_skipped; }

extern "C" void matter_svc_retry(void)
{
    guard_set(0);
    esp_restart();
}
extern "C" uint8_t matter_svc_fabrics(void) { return s_fabrics; }

extern "C" const char *matter_svc_state_text(vd_mt_state_t st)
{
    static const char *T[] = { "Off", "Starting", "Ready to pair", "Paired", "Error" };
    return st <= MT_ERROR ? T[st] : "?";
}

extern "C" bool matter_svc_qr(char *buf, size_t n)
{
    if (!s_qr[0]) return false;
    strlcpy(buf, s_qr, n);
    return true;
}

extern "C" bool matter_svc_manual_code(char *buf, size_t n)
{
    if (!s_manual[0]) return false;
    // 1234-567-8901, as Apple Home prints it
    snprintf(buf, n, "%.4s-%.3s-%.4s", s_manual, s_manual + 4, s_manual + 7);
    return true;
}

extern "C" void matter_svc_reset(void)
{
    if (s_state == MT_OFF || s_state == MT_ERROR) return;
    ESP_LOGW(TAG, "forgetting all controllers");
    esp_matter::factory_reset();   // erases the Matter fabrics (not the setup code) and restarts
}
