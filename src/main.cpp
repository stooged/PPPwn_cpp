#include <iostream>
#include <fstream>
#include <unordered_map>
#include <vector>
#include <string>
#include <PcapLiveDeviceList.h>
#include <clipp.h>
#include <signal.h>

#if defined(__APPLE__)

#include <SystemConfiguration/SystemConfiguration.h>

#endif

#include "exploit.h"

void listInterfaces() {
    std::cout << "[+] interfaces: " << std::endl;
#if defined(__APPLE__)
    CFArrayRef interfaces = SCNetworkInterfaceCopyAll();
    if (!interfaces) {
        std::cout << "[-] Failed to get interfaces" << std::endl;
        exit(1);
    }
    CFIndex serviceCount = CFArrayGetCount(interfaces);
    char buffer[1024];
    for (CFIndex i = 0; i < serviceCount; ++i) {
        auto interface = (SCNetworkInterfaceRef) CFArrayGetValueAtIndex(interfaces, i);
        auto serviceName = SCNetworkInterfaceGetLocalizedDisplayName(interface);
        auto bsdName = SCNetworkInterfaceGetBSDName(interface);
        if (bsdName) {
            CFStringGetCString(bsdName, buffer, sizeof(buffer), kCFStringEncodingUTF8);
            printf("\t%s ", buffer);
            if (serviceName) {
                CFStringGetCString(serviceName, buffer, sizeof(buffer), kCFStringEncodingUTF8);
                printf("%s", buffer);
            }
            printf("\n");
        }
    }
    CFRelease(interfaces);
#else
    std::vector<pcpp::PcapLiveDevice *> devList = pcpp::PcapLiveDeviceList::getInstance().getPcapLiveDevicesList();
    for (pcpp::PcapLiveDevice *dev: devList) {
        if (dev->getLoopback()) continue;
        std::cout << "\t" << dev->getName() << " " << dev->getDesc() << std::endl;
    }
#endif
    exit(0);
}

enum FirmwareVersion getFirmwareOffset(int fw) {
    std::unordered_map<int, enum FirmwareVersion> fw_choices = {
            {700,  FIRMWARE_700_702},
            {701,  FIRMWARE_700_702},
            {702,  FIRMWARE_700_702},
            {750,  FIRMWARE_750_755},
            {750,  FIRMWARE_750_755},
            {751,  FIRMWARE_750_755},
            {755,  FIRMWARE_750_755},
            {800,  FIRMWARE_800_803},
            {801,  FIRMWARE_800_803},
            {803,  FIRMWARE_800_803},
            {850,  FIRMWARE_850_852},
            {852,  FIRMWARE_850_852},
            {900,  FIRMWARE_900},
            {903,  FIRMWARE_903_904},
            {904,  FIRMWARE_903_904},
            {950,  FIRMWARE_950_960},
            {951,  FIRMWARE_950_960},
            {960,  FIRMWARE_950_960},
            {1000, FIRMWARE_1000_1001},
            {1001, FIRMWARE_1000_1001},
            {1050, FIRMWARE_1050_1071},
            {1070, FIRMWARE_1050_1071},
            {1071, FIRMWARE_1050_1071},
            {1100, FIRMWARE_1100}
    };
    if (fw_choices.count(fw) == 0) return FIRMWARE_UNKNOWN;
    return fw_choices[fw];
}

#define SUPPORTED_FIRMWARE "{700,701,702,750,751,755,800,801,803,850,852,900,903,904,950,951,960,1000,1001,1050,1070,1071,1100} (default: 1100)"

static std::shared_ptr<Exploit> exploit = std::make_shared<Exploit>();

static void signal_handler(int sig_num) {
    signal(sig_num, signal_handler);
    exploit->ppp_byebye();
    exit(sig_num);
}

int main(int argc, char *argv[]) {
    using namespace clipp;
    std::cout << "[+] PPPwn++ - PlayStation 4 PPPoE RCE by theflow" << std::endl;
    std::string interface;
    int fw = 1100;
    int timeout = 0;
    int wait_after_pin = 1;
    int groom_delay = 4;
    int buffer_size = 0;
    std::string ipv6_addr = "fe80::9f9f:41ff:9f9f:41ff";
    bool retry = false;
    bool no_wait_padi = false;
    bool real_sleep = false;
    bool use_gh = false;

    auto cli = (
            ("network interface" % required("-i", "--interface") & value("interface", interface), \
            SUPPORTED_FIRMWARE % option("--fw") & integer("fw", fw), \
            "set the ipv6 source address used (default: fe80::9f9f:41ff:9f9f:41ff)\n" %
            option("--ipv") & value("fe80::9f9f:41ff:9f9f:41ff", ipv6_addr), \
            "Use GoldHen if available for selected firmware (default: vtx-hen)" %
            option("-gh", "--use-goldhen").set(use_gh), \
            "timeout in seconds for ps4 response, 0 means always wait (default: 0)" %
            option("-t", "--timeout") & integer("seconds", timeout), \
            "Waiting time in seconds after the first round CPU pinning (default: 1)" %
            option("-wap", "--wait-after-pin") & integer("seconds", wait_after_pin), \
            "wait for 1ms every `n` rounds during Heap grooming (default: 4)" % option("-gd", "--groom-delay") &
            integer("1-4097", groom_delay), \
            "PCAP buffer size in bytes, less than 100 indicates default value (usually 2MB)  (default: 0)" %
            option("-bs", "--buffer-size") & integer("bytes", buffer_size), \
            "automatically retry when fails or timeout" % option("-a", "--auto-retry").set(retry), \
            "don't wait one more PADI before starting" % option("-nw", "--no-wait-padi").set(no_wait_padi), \
            "Use CPU for more precise sleep time (Only used when execution speed is too slow)" %
            option("-rs", "--real-sleep").set(real_sleep)
            ) | \
            "list interfaces" % command("list").call(listInterfaces)
    );

    auto result = parse(argc, argv, cli);
    if (!result) {
        std::cout << make_man_page(cli, "pppwn");
        return 1;
    }

    auto offset = getFirmwareOffset(fw);
    if (offset == FIRMWARE_UNKNOWN) {
        std::cout << "[-] Invalid firmware version" << std::endl;
        std::cout << make_man_page(cli, "pppwn");
        return 1;
    }

    std::cout << "[+] args: interface=" << interface << " fw=" << fw << " ipv=" << ipv6_addr << " gh=" << (use_gh ? "on" : "off")
              << " timeout=" << timeout << " wait-after-pin=" << wait_after_pin << " groom-delay=" << groom_delay
              << " auto-retry=" << (retry ? "on" : "off") << " no-wait-padi=" << (no_wait_padi ? "on" : "off")
              << " real_sleep=" << (real_sleep ? "on" : "off")
              << std::endl;

    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    if (exploit->setFirmwareVersion((FirmwareVersion) offset)) return 1;
    if (exploit->setInterface(interface, buffer_size)) return 1;
    exploit->setUseGH(fw, use_gh);
    exploit->setIpv6(ipv6_addr);
    exploit->setTimeout(timeout);
    exploit->setWaitPADI(!no_wait_padi);
    exploit->setGroomDelay(groom_delay);
    exploit->setWaitAfterPin(wait_after_pin);
    exploit->setAutoRetry(retry);
    exploit->setRealSleep(real_sleep);

    return exploit->run();
}
