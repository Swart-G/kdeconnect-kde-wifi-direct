// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
#include "wifidirectcapabilities.h"
#include <KLocalizedString>
#include <QNetworkInterface>

#ifdef Q_OS_LINUX
#include <linux/nl80211.h>
#include <netlink/genl/ctrl.h>
#include <netlink/genl/genl.h>
#include <netlink/msg.h>
#include <sys/socket.h>
#include <sys/time.h>
#endif

namespace WifiDirectCapabilities
{
#ifdef Q_OS_LINUX
static bool fits(QList<int> types, QList<Limit> limits, int index = 0)
{
    if (index == types.size()) return true;
    for (int i = 0; i < limits.size(); ++i) {
        if (limits[i].maximum > 0 && limits[i].types.contains(types[index])) {
            --limits[i].maximum;
            if (fits(types, limits, index + 1)) return true;
            ++limits[i].maximum;
        }
    }
    return false;
}
#endif

Result evaluate(const Radio &radio)
{
    Result result;
    result.stationConnected = radio.stationConnected;
#ifdef Q_OS_LINUX
    if (!radio.modes.contains(NL80211_IFTYPE_P2P_CLIENT) || !radio.modes.contains(NL80211_IFTYPE_P2P_GO)) {
        result.reason = i18n("The Wi-Fi driver does not support both Wi-Fi Direct roles.");
        return result;
    }
    if (radio.running.value(NL80211_IFTYPE_P2P_CLIENT) || radio.running.value(NL80211_IFTYPE_P2P_GO)) {
        result.reason = i18n("Another Wi-Fi Direct group is active on this adapter.");
        return result;
    }
    for (int role : {NL80211_IFTYPE_P2P_CLIENT, NL80211_IFTYPE_P2P_GO}) {
        QList<int> types{role};
        for (auto it = radio.running.cbegin(); it != radio.running.cend(); ++it) {
            if (!radio.softwareModes.contains(it.key())) {
                for (int n = 0; n < it.value(); ++n) types.append(it.key());
            }
        }
        bool allowed = types.size() == 1 && !radio.stationConnected;
        for (const auto &combination : radio.combinations) {
            const int channels = qMax(radio.stationConnected ? 2 : 1, radio.occupiedChannels + 1);
            if (combination.maximum >= types.size() && combination.channels >= channels
                && fits(types, combination.limits)) {
                allowed = true;
                break;
            }
        }
        if (!allowed) {
            result.reason = i18n("The driver cannot guarantee ordinary Wi-Fi together with both Wi-Fi Direct roles on two channels.");
            return result;
        }
    }
    result.available = true;
#else
    Q_UNUSED(radio);
    result.reason = i18n("Wi-Fi Direct capability checking is only available on Linux.");
#endif
    return result;
}

#ifdef Q_OS_LINUX
struct Interface {
    QString name;
    int radio = -1;
    int type = 0;
    uint frequency = 0;
};
struct Dump {
    QMap<int, Radio> radios;
    QList<Interface> interfaces;
    int pending = 1;
};

static int readInterface(nl_msg *message, void *data)
{
    auto *dump = static_cast<Dump *>(data);
    nlattr *attrs[NL80211_ATTR_MAX + 1]{};
    if (genlmsg_parse(nlmsg_hdr(message), 0, attrs, NL80211_ATTR_MAX, nullptr) < 0) return NL_SKIP;
    if (!attrs[NL80211_ATTR_WIPHY] || !attrs[NL80211_ATTR_IFTYPE]) return NL_SKIP;
    Interface iface;
    iface.radio = nla_get_u32(attrs[NL80211_ATTR_WIPHY]);
    iface.type = nla_get_u32(attrs[NL80211_ATTR_IFTYPE]);
    if (attrs[NL80211_ATTR_IFNAME]) iface.name = QString::fromUtf8(nla_get_string(attrs[NL80211_ATTR_IFNAME]));
    if (attrs[NL80211_ATTR_WIPHY_FREQ]) iface.frequency = nla_get_u32(attrs[NL80211_ATTR_WIPHY_FREQ]);
    dump->interfaces.append(iface);
    return NL_OK;
}

static int readRadio(nl_msg *message, void *data)
{
    auto *dump = static_cast<Dump *>(data);
    nlattr *attrs[NL80211_ATTR_MAX + 1]{};
    if (genlmsg_parse(nlmsg_hdr(message), 0, attrs, NL80211_ATTR_MAX, nullptr) < 0 || !attrs[NL80211_ATTR_WIPHY]) return NL_SKIP;
    auto &radio = dump->radios[nla_get_u32(attrs[NL80211_ATTR_WIPHY])];
    nlattr *entry;
    int remaining;
    if (attrs[NL80211_ATTR_SUPPORTED_IFTYPES]) {
        nla_for_each_nested(entry, attrs[NL80211_ATTR_SUPPORTED_IFTYPES], remaining) radio.modes.insert(nla_type(entry));
    }
    if (attrs[NL80211_ATTR_SOFTWARE_IFTYPES]) {
        nla_for_each_nested(entry, attrs[NL80211_ATTR_SOFTWARE_IFTYPES], remaining) radio.softwareModes.insert(nla_type(entry));
    }
    if (attrs[NL80211_ATTR_INTERFACE_COMBINATIONS]) {
        nla_for_each_nested(entry, attrs[NL80211_ATTR_INTERFACE_COMBINATIONS], remaining) {
            nlattr *comb[NUM_NL80211_IFACE_COMB]{};
            if (nla_parse_nested(comb, MAX_NL80211_IFACE_COMB, entry, nullptr) < 0
                || !comb[NL80211_IFACE_COMB_MAXNUM] || !comb[NL80211_IFACE_COMB_NUM_CHANNELS] || !comb[NL80211_IFACE_COMB_LIMITS]) continue;
            Combination combination;
            combination.maximum = nla_get_u32(comb[NL80211_IFACE_COMB_MAXNUM]);
            combination.channels = nla_get_u32(comb[NL80211_IFACE_COMB_NUM_CHANNELS]);
            nlattr *limitEntry;
            int limitRemaining;
            nla_for_each_nested(limitEntry, comb[NL80211_IFACE_COMB_LIMITS], limitRemaining) {
                nlattr *lim[NUM_NL80211_IFACE_LIMIT]{};
                if (nla_parse_nested(lim, MAX_NL80211_IFACE_LIMIT, limitEntry, nullptr) < 0
                    || !lim[NL80211_IFACE_LIMIT_MAX] || !lim[NL80211_IFACE_LIMIT_TYPES]) continue;
                Limit limit;
                limit.maximum = nla_get_u32(lim[NL80211_IFACE_LIMIT_MAX]);
                nlattr *type;
                int typeRemaining;
                nla_for_each_nested(type, lim[NL80211_IFACE_LIMIT_TYPES], typeRemaining) limit.types.insert(nla_type(type));
                combination.limits.append(limit);
            }
            radio.combinations.append(combination);
        }
    }
    return NL_OK;
}

static bool request(nl_sock *socket, int family, int command, Dump &dump, nl_recvmsg_msg_cb_t callback)
{
    nl_msg *message = nlmsg_alloc();
    if (!message) return false;
    genlmsg_put(message, NL_AUTO_PORT, NL_AUTO_SEQ, family, 0, NLM_F_DUMP, command, 0);
    if (command == NL80211_CMD_GET_WIPHY) nla_put_flag(message, NL80211_ATTR_SPLIT_WIPHY_DUMP);
    dump.pending = 1;
    nl_socket_modify_cb(socket, NL_CB_VALID, NL_CB_CUSTOM, callback, &dump);
    nl_socket_modify_cb(socket, NL_CB_FINISH, NL_CB_CUSTOM, [](nl_msg *, void *data) -> int {
        static_cast<Dump *>(data)->pending = 0;
        return NL_SKIP;
    }, &dump);
    nl_socket_modify_err_cb(socket, NL_CB_CUSTOM, [](sockaddr_nl *, nlmsgerr *error, void *data) -> int {
        static_cast<Dump *>(data)->pending = error->error;
        return NL_STOP;
    }, &dump);
    int error = nl_send_auto(socket, message);
    nlmsg_free(message);
    while (error >= 0 && dump.pending > 0) error = nl_recvmsgs_default(socket);
    return error >= 0 && dump.pending == 0;
}
#endif

Result probe(const QString &interfaceName)
{
#ifdef Q_OS_LINUX
    Result result;
    result.reason = i18n("Cannot reliably read Wi-Fi driver capabilities through nl80211.");
    nl_sock *socket = nl_socket_alloc();
    if (!socket) return result;
    Dump dump;
    bool ok = genl_connect(socket) >= 0;
    if (ok) {
        timeval timeout{2, 0};
        setsockopt(nl_socket_get_fd(socket), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        int family = genl_ctrl_resolve(socket, "nl80211");
        ok = family >= 0 && request(socket, family, NL80211_CMD_GET_INTERFACE, dump, readInterface)
            && request(socket, family, NL80211_CMD_GET_WIPHY, dump, readRadio);
    }
    nl_socket_free(socket);
    if (!ok) return result;
    int selected = -1;
    const QString underlying = interfaceName.startsWith(QLatin1String("p2p-dev-")) ? interfaceName.mid(8) : interfaceName;
    for (const auto &iface : dump.interfaces) {
        if (iface.name == interfaceName || iface.name == underlying) selected = iface.radio;
    }
    if (selected < 0 || !dump.radios.contains(selected)) return result;
    auto radio = dump.radios.value(selected);
    QStringList stations;
    QSet<uint> frequencies;
    for (const auto &iface : dump.interfaces) {
        if (iface.radio != selected) continue;
        const auto net = QNetworkInterface::interfaceFromName(iface.name);
        if (iface.frequency || net.flags().testFlag(QNetworkInterface::IsUp)) {
            ++radio.running[iface.type];
            if (iface.frequency) frequencies.insert(iface.frequency);
            if (iface.type == NL80211_IFTYPE_STATION && iface.frequency) {
                radio.stationConnected = true;
                stations.append(iface.name);
            }
        }
    }
    radio.occupiedChannels = frequencies.size();
    result = evaluate(radio);
    result.stationInterfaces = stations;
    return result;
#else
    Q_UNUSED(interfaceName);
    return evaluate({});
#endif
}
}
