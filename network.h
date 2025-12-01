#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <sys/socket.h>
#include <string.h>
#include <errno.h>
#include <linux/if_link.h>
#include <sched.h>
#include <linux/if.h>
#include <sys/ioctl.h>

// Create a socket to listen for network events
int nl_socket()
{
    int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    if (fd < 0)
    {
        perror("socket NETLINK_ROUTE");
        exit(1);
    }
    return fd;
}

int move_if_to_ns(const char *ifname, pid_t child_pid)
{
    char path[256];
    snprintf(path, sizeof(path), "/proc/%d/ns/net", child_pid);

    int netns = open(path, O_RDONLY);
    if (netns < 0)
    {
        perror("open netns");
        return -1;
    }

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strcpy(ifr.ifr_name, ifname);
    ifr.ifr_ifindex = if_nametoindex(ifname);

    if (ioctl(fd, SIOCSIFNETNS, &ifr) < 0)
    {
        perror("SIOCSIFNETNS");
        return -1;
    }
    close(fd);
    close(netns);
    return 0;
}

// networking slop to create a veth pair
int create_veth(const char *host_if, const char *cont_if)
{
    int fd = nl_socket();

    char buf[4096];
    struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
    struct ifinfomsg *ifi;

    memset(buf, 0, sizeof(buf));

    nlh->nlmsg_len = NLMSG_LENGTH(sizeof(*ifi));
    nlh->nlmsg_type = RTM_NEWLINK;
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_EXCL;
    nlh->nlmsg_seq = 1;

    ifi = NLMSG_DATA(nlh);
    ifi->ifi_family = AF_UNSPEC;

    // nest attributes
    struct rtattr *linkinfo = (struct rtattr *)(((char *)nlh) + NLMSG_ALIGN(nlh->nlmsg_len));
    linkinfo->rta_type = IFLA_LINKINFO;
    linkinfo->rta_len = RTA_LENGTH(0);

    struct rtattr *kind = (struct rtattr *)(((char *)linkinfo) + RTA_ALIGN(linkinfo->rta_len));
    kind->rta_type = IFLA_INFO_KIND;
    kind->rta_len = RTA_LENGTH(strlen("veth") + 1);
    strcpy((char *)RTA_DATA(kind), "veth");

    linkinfo->rta_len = (char *)kind + kind->rta_len - (char *)linkinfo;

    // veth pair attributes
    struct rtattr *data = (struct rtattr *)(((char *)linkinfo) + RTA_ALIGN(linkinfo->rta_len));
    data->rta_type = IFLA_INFO_DATA;
    data->rta_len = RTA_LENGTH(0);

    struct rtattr *peer = (struct rtattr *)(((char *)data) + RTA_ALIGN(data->rta_len));
    peer->rta_type = VETH_INFO_PEER;
    peer->rta_len = RTA_LENGTH(sizeof(struct ifinfomsg) + strlen(cont_if) + 1);

    struct ifinfomsg *peer_msg = (struct ifinfomsg *)RTA_DATA(peer);
    peer_msg->ifi_family = AF_UNSPEC;

    // peer name
    struct rtattr *peer_name = (struct rtattr *)(((char *)peer_msg) + NLMSG_ALIGN(sizeof(*peer_msg)));
    peer_name->rta_type = IFLA_IFNAME;
    peer_name->rta_len = RTA_LENGTH(strlen(cont_if) + 1);
    strcpy((char *)RTA_DATA(peer_name), cont_if);

    peer->rta_len = (char *)peer_name + peer_name->rta_len - (char *)peer;

    data->rta_len = (char *)peer + peer->rta_len - (char *)data;
    linkinfo->rta_len = (char *)data + data->rta_len - (char *)linkinfo;

    nlh->nlmsg_len = NLMSG_ALIGN(nlh->nlmsg_len) + RTA_ALIGN(linkinfo->rta_len);

    // host interface name
    struct rtattr *ifname = (struct rtattr *)(((char *)nlh) + NLMSG_ALIGN(nlh->nlmsg_len));
    ifname->rta_type = IFLA_IFNAME;
    ifname->rta_len = RTA_LENGTH(strlen(host_if) + 1);
    strcpy((char *)RTA_DATA(ifname), host_if);

    nlh->nlmsg_len = NLMSG_ALIGN(nlh->nlmsg_len) + RTA_ALIGN(ifname->rta_len);

    // send
    if (send(fd, nlh, nlh->nlmsg_len, 0) < 0)
    {
        perror("send netlink");
        return -1;
    }

    return 0;
}
