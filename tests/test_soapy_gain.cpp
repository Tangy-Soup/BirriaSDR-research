/* Host-only: actual module + device.c, injected backend, no USB implementation. */
#include "../soapy/BirriaSDR.cpp"
#include <cstdlib>
#define CHECK(t) do { if (!(t)) { std::cerr << "line " << __LINE__ << ": " << #t << "\n"; std::exit(1); } } while (0)
#include <iostream>
extern "C" {
void mock_fail_second_live_gain(); void mock_fail_first_live_gain();
int mock_recovered(); int mock_running(); void mock_feed_level(int);
void mock_committed(int*,int*,int*,int*);
}
static void recovered() {
    for (int i=0;i<1000&&!mock_recovered();++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(mock_recovered());
}
static void codes(BirriaSDRDevice &d,int l,int p,int v,int m) {
    int a,b,c,n; mock_committed(&a,&b,&c,&n);
    CHECK(a==l&&b==p&&c==v&&n==m);
    CHECK(d.getGainMode(SOAPY_SDR_RX,0)==!m);
    if (m) {
        CHECK(d.getGain(SOAPY_SDR_RX,0,"LNA")==l);
        CHECK(d.getGain(SOAPY_SDR_RX,0,"LPF")==p);
        CHECK(d.getGain(SOAPY_SDR_RX,0,"DIG")==v);
    }
}
/* Automatic mode's host loop against the real step tables, on a fake stream. */
static void host_agc(const birriasdr_device_info &info) {
    BirriaSDRDevice d(0,info);
    d.writeSetting("auto_gain","host");
    CHECK(d.readSetting("auto_gain")=="host"); CHECK(d.getGainMode(SOAPY_SDR_RX,0));
    d.setGain(SOAPY_SDR_RX,0,"LNA",2); d.setGain(SOAPY_SDR_RX,0,"LPF",3);
    int a,b,c,n; mock_committed(&a,&b,&c,&n);
    CHECK(a==5&&b==8&&c==0&&n==1); // the top analog step, no digital; library held manual
    double top=d.getGain(SOAPY_SDR_RX,0);
    auto *s=d.setupStream(SOAPY_SDR_RX,SOAPY_SDR_CS16,{},{});
    CHECK(!d.activateStream(s,0,0,0));
    for(int i=0;i<1000&&!mock_running();++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    // Quiet: no change over a down window.
    mock_feed_level(100);
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    CHECK(d.getGain(SOAPY_SDR_RX,0)==top);
    // Full scale: drops at least 6 dB, stays automatic.
    mock_feed_level(8191);
    double now=top;
    for(int i=0;i<40&&now==top;++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        now=d.getGain(SOAPY_SDR_RX,0);
    }
    CHECK(now<=top-6.0); CHECK(d.getGainMode(SOAPY_SDR_RX,0));
    mock_feed_level(0);
    // Manual returns to the sliders, not to what the loop held, and the loop stops.
    d.setGainMode(SOAPY_SDR_RX,0,false);
    mock_committed(&a,&b,&c,&n); CHECK(a==2&&b==3&&c==0&&n==1);
    mock_feed_level(8191);
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    mock_committed(&a,&b,&c,&n); CHECK(a==2&&b==3&&c==0&&n==1);
    mock_feed_level(0);
    // Released: the chip owns the gain.
    d.setGainMode(SOAPY_SDR_RX,0,true);
    d.writeSetting("auto_gain","released");
    mock_committed(&a,&b,&c,&n); CHECK(n==0);
    d.writeSetting("auto_gain","host");
    // A live retune into 5 GHz climbs to its top, and back out lowers the LPF first.
    d.setFrequency(SOAPY_SDR_RX,0,"RF",5240e6,{});
    mock_committed(&a,&b,&c,&n); CHECK(a==5&&b>8&&n==1);
    d.setFrequency(SOAPY_SDR_RX,0,"RF",2437e6,{});
    CHECK(d.getFrequency(SOAPY_SDR_RX,0)==2437e6);
    mock_committed(&a,&b,&c,&n); CHECK(a==5&&b==8&&n==1);
    CHECK(d.getGain(SOAPY_SDR_RX,0,"LPF")==8); // reads report what the loop holds
    d.setGainMode(SOAPY_SDR_RX,0,false);
    mock_committed(&a,&b,&c,&n); CHECK(a==2&&b==3&&n==1); // the slider was kept for manual
    CHECK(d.getGain(SOAPY_SDR_RX,0,"LPF")==3);
    d.deactivateStream(s,0,0); d.closeStream(s);
    d.writeSetting("auto_gain","released");
    bool threw=false;
    try { d.writeSetting("auto_gain","bogus"); } catch (const std::exception &) { threw=true; }
    CHECK(threw);
    std::cout<<"PASS automatic host gain loop\n";
}
int main() {
    birriasdr_device_info info{}; CHECK(!birriasdr_get_device_info(0,&info));
    host_agc(info);
    BirriaSDRDevice d(0,info);
    CHECK(d.readSetting("auto_gain")=="host"); // the default; a new device holds the top step
    int la,lb,lc,ln; mock_committed(&la,&lb,&lc,&ln); CHECK(la==5&&lb==8&&!lc&&ln==1);
    // The rest covers staging in released mode, from the staged 1/4/0.
    d.writeSetting("auto_gain","released");
    d.setGainMode(SOAPY_SDR_RX,0,false); d.setGainMode(SOAPY_SDR_RX,0,true);
    auto *s=d.setupStream(SOAPY_SDR_RX,SOAPY_SDR_CS16,{},{});
    CHECK(!d.activateStream(s,0,0,0));
    for(int i=0;i<1000&&!mock_running();++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(mock_running());
    d.setGain(SOAPY_SDR_RX,0,"LNA",5); d.setGain(SOAPY_SDR_RX,0,"LPF",8);
    CHECK(d.getGain(SOAPY_SDR_RX,0,"LNA")==5); codes(d,1,4,0,0);
    mock_fail_first_live_gain(); d.setGainMode(SOAPY_SDR_RX,0,false); recovered();
    codes(d,1,4,0,0); CHECK(d.getGain(SOAPY_SDR_RX,0,"LNA")==5);
    mock_fail_second_live_gain(); d.setGainMode(SOAPY_SDR_RX,0,false); recovered();
    codes(d,1,4,0,1);
    d.setGain(SOAPY_SDR_RX,0,"DIG",1); codes(d,1,4,1,1);
    std::cout<<"PASS first transaction failure preserves staging; second failure reports committed 1/4/0; DIG-only => 1/4/1\n";
    // Direct manual failure must also preserve the committed tuple.
    mock_fail_first_live_gain(); d.setGain(SOAPY_SDR_RX,0,"LNA",5); recovered();
    codes(d,1,4,1,1);
    d.setGain(SOAPY_SDR_RX,0,"DIG",2); codes(d,1,4,2,1);
    // A pending overall dB request survives a failed application and can retry.
    d.setGainMode(SOAPY_SDR_RX,0,true);
    d.setGain(SOAPY_SDR_RX,0,30.0); CHECK(d.getGain(SOAPY_SDR_RX,0)==30.0);
    mock_fail_second_live_gain(); d.setGainMode(SOAPY_SDR_RX,0,false); recovered();
    codes(d,1,4,2,1); CHECK(d.getGain(SOAPY_SDR_RX,0)!=30.0);
    d.setGainMode(SOAPY_SDR_RX,0,false);
    double actual=d.getGain(SOAPY_SDR_RX,0); CHECK(std::abs(actual-30.0)<2.0);
    std::cout<<"PASS pending dB survives failure; retry reports committed table step "<<actual<<"\n";
    d.setGainMode(SOAPY_SDR_RX,0,true);d.setGain(SOAPY_SDR_RX,0,10.0);
    mock_fail_second_live_gain();d.setGainMode(SOAPY_SDR_RX,0,false);recovered();
    double maximum=d.getGainRange(SOAPY_SDR_RX,0).maximum();
    d.setGain(SOAPY_SDR_RX,0,maximum);d.setGainMode(SOAPY_SDR_RX,0,false);
    CHECK(d.getGain(SOAPY_SDR_RX,0)==maximum); // successful manual override clears pending
    d.deactivateStream(s,0,0);d.closeStream(s);
    // Real gain tables and band/chain validation, with no live hardware I/O.
    for (size_t chain: {0u,1u}) {
        s=d.setupStream(SOAPY_SDR_RX,SOAPY_SDR_CS16,{chain},{});
        d.setGainMode(SOAPY_SDR_RX,0,true);
        d.setFrequency(SOAPY_SDR_RX,0,"RF",5240e6,{});
        CHECK(d.getGainRange(SOAPY_SDR_RX,0,"LPF").maximum()==15);
        d.setGain(SOAPY_SDR_RX,0,"LPF",14); d.setGainMode(SOAPY_SDR_RX,0,false);
        CHECK(d.getGain(SOAPY_SDR_RX,0,"LPF")==14);
        double maximum=d.getGainRange(SOAPY_SDR_RX,0).maximum();
        d.setGain(SOAPY_SDR_RX,0,maximum);
        CHECK(d.getGain(SOAPY_SDR_RX,0)==maximum);
        d.setFrequency(SOAPY_SDR_RX,0,"RF",2437e6,{});
        CHECK(d.getGainRange(SOAPY_SDR_RX,0,"LPF").maximum()==8);
        CHECK(d.getGain(SOAPY_SDR_RX,0,"LPF")<=8);
        // A retune can fit committed codes. A DIG-only edit must use those codes.
        d.setGain(SOAPY_SDR_RX,0,"DIG",1);
        CHECK(d.getGain(SOAPY_SDR_RX,0,"LPF")<=8);
        d.closeStream(s);
    }
    std::cout<<"PASS real per-band/per-chain gain tables and LPF limits\n";
}
