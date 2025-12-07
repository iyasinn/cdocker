#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/if_link.h>
#include <linux/veth.h>
#include <linux/if_addr.h>
#include <sys/socket.h>




/*
 * ============================================================
 * PART 1: NETLINK SOCKET HELPERS
 * ============================================================
 */

// Create and bind a netlink socket for route operations
int nl_open(void)
{
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0)
    {
        perror("socket(NETLINK_ROUTE)");
        return -1;
    }

    struct sockaddr_nl sa = {
        .nl_family = AF_NETLINK,
        .nl_pid = 0,    // let kernel assign
        .nl_groups = 0  // no multicast
    };

    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0)
    {
        perror("bind(netlink)");
        close(fd);
        return -1;
    }

    return fd;
}

// Send a netlink message and wait for ACK
// Returns 0 on success, -errno on failure
int nl_send_and_wait(int fd, struct nlmsghdr *nlh)
{
    // Send the message
    if (send(fd, nlh, nlh->nlmsg_len, 0) < 0)
    {
        perror("send(netlink)");
        return -errno;
    }

    // Receive response
    char buf[4096];
    int len = recv(fd, buf, sizeof(buf), 0);
    if (len < 0)
    {
        perror("recv(netlink)");
        return -errno;
    }

    // Parse response
    struct nlmsghdr *resp = (struct nlmsghdr *)buf;
    if (resp->nlmsg_type == NLMSG_ERROR)
    {
        struct nlmsgerr *err = NLMSG_DATA(resp);
        if (err->error != 0)
        {
            errno = -err->error;
            return err->error;  // negative error code
        }
        // error == 0 means ACK (success)
    }

    return 0;
}

/*
 * ============================================================
 * PART 2: ATTRIBUTE BUILDING HELPERS
 * 
 * Netlink messages are built by appending "rtattr" structures.
 * Each attribute has a type, length, and payload.
 * Some attributes are "nested" - they contain other attributes.
 * ============================================================
 */

// Helper struct to track buffer position while building messages
struct nl_msg {
    char *buf;           // buffer start
    size_t size;         // buffer capacity
    struct nlmsghdr *nlh; // points to start (same as buf)
};

// Get pointer to current end of message (where next attr goes)
static inline void *nl_tail(struct nl_msg *msg)
{
    return (char *)msg->nlh + NLMSG_ALIGN(msg->nlh->nlmsg_len);
}

// Add a simple attribute with arbitrary data
struct rtattr *nl_attr_put(struct nl_msg *msg, int type, const void *data, size_t len)
{
    struct rtattr *attr = nl_tail(msg);
    attr->rta_type = type;
    attr->rta_len = RTA_LENGTH(len);

    if (data && len)
    {
        memcpy(RTA_DATA(attr), data, len);
    }

    msg->nlh->nlmsg_len += RTA_ALIGN(attr->rta_len);
    return attr;
}

// Add a string attribute (includes null terminator)
struct rtattr *nl_attr_put_str(struct nl_msg *msg, int type, const char *str)
{
    return nl_attr_put(msg, type, str, strlen(str) + 1);
}

// Add a u32 attribute
struct rtattr *nl_attr_put_u32(struct nl_msg *msg, int type, uint32_t val)
{
    return nl_attr_put(msg, type, &val, sizeof(val));
}

// Start a nested attribute (returns pointer to update length later)
struct rtattr *nl_attr_nest_start(struct nl_msg *msg, int type)
{
    struct rtattr *nest = nl_tail(msg);
    nest->rta_type = type;
    nest->rta_len = RTA_LENGTH(0);  // will be updated by nest_end
    msg->nlh->nlmsg_len += RTA_ALIGN(nest->rta_len);
    return nest;
}

// Close a nested attribute (updates its length to include all children)
void nl_attr_nest_end(struct nl_msg *msg, struct rtattr *nest)
{
    nest->rta_len = (char *)nl_tail(msg) - (char *)nest;
}

/*
 * ============================================================
 * PART 3: VETH PAIR CREATION
 * ============================================================
 */

int veth_create(const char *name1, const char *name2)
{
    int fd = nl_open();
    if (fd < 0) return -1;

    // Allocate message buffer
    char buf[4096];
    memset(buf, 0, sizeof(buf));

    struct nl_msg msg = {
        .buf = buf,
        .size = sizeof(buf),
        .nlh = (struct nlmsghdr *)buf
    };

    // Initialize netlink header
    msg.nlh->nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
    msg.nlh->nlmsg_type = RTM_NEWLINK;
    msg.nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_EXCL | NLM_F_ACK;
    msg.nlh->nlmsg_seq = 1;

    // Initialize interface info (for first interface)
    struct ifinfomsg *ifi = NLMSG_DATA(msg.nlh);
    ifi->ifi_family = AF_UNSPEC;

    // Name the first interface
    nl_attr_put_str(&msg, IFLA_IFNAME, name1);

    // Start IFLA_LINKINFO nest
    struct rtattr *linkinfo = nl_attr_nest_start(&msg, IFLA_LINKINFO);

    // Specify link type as "veth"
    nl_attr_put_str(&msg, IFLA_INFO_KIND, "veth");

    // Start IFLA_INFO_DATA nest (veth-specific configuration)
    struct rtattr *info_data = nl_attr_nest_start(&msg, IFLA_INFO_DATA);

    // Start VETH_INFO_PEER nest (peer interface configuration)
    struct rtattr *peer = nl_attr_nest_start(&msg, VETH_INFO_PEER);

    // Peer needs its own ifinfomsg header (this is a quirk of veth)
    struct ifinfomsg *peer_ifi = nl_tail(&msg);
    memset(peer_ifi, 0, sizeof(*peer_ifi));
    peer_ifi->ifi_family = AF_UNSPEC;
    msg.nlh->nlmsg_len += sizeof(struct ifinfomsg);

    // Name the peer interface
    nl_attr_put_str(&msg, IFLA_IFNAME, name2);

    // Close all nests (inside-out order!)
    nl_attr_nest_end(&msg, peer);
    nl_attr_nest_end(&msg, info_data);
    nl_attr_nest_end(&msg, linkinfo);

    // Send and check result
    int ret = nl_send_and_wait(fd, msg.nlh);
    if (ret < 0)
    {
        fprintf(stderr, "veth_create failed: %s\n", strerror(-ret));
    }

    close(fd);
    return ret;
}

/*
 * ============================================================
 * PART 4: MOVE INTERFACE TO NAMESPACE
 * ============================================================
 */

int if_move_to_pid_ns(const char *ifname, pid_t pid)
{
    int fd = nl_open();
    if (fd < 0) return -1;

    unsigned int ifindex = if_nametoindex(ifname);
    if (ifindex == 0)
    {
        fprintf(stderr, "Interface %s not found\n", ifname);
        close(fd);
        return -ENODEV;
    }

    char buf[4096];
    memset(buf, 0, sizeof(buf));

    struct nl_msg msg = {
        .buf = buf,
        .size = sizeof(buf),
        .nlh = (struct nlmsghdr *)buf
    };

    msg.nlh->nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
    msg.nlh->nlmsg_type = RTM_NEWLINK;  // modify existing link
    msg.nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    msg.nlh->nlmsg_seq = 1;

    struct ifinfomsg *ifi = NLMSG_DATA(msg.nlh);
    ifi->ifi_family = AF_UNSPEC;
    ifi->ifi_index = ifindex;  // which interface to modify

    // IFLA_NET_NS_PID tells kernel: move this interface to the
    // network namespace of process with this PID
    nl_attr_put(&msg, IFLA_NET_NS_PID, &pid, sizeof(pid));

    int ret = nl_send_and_wait(fd, msg.nlh);
    if (ret < 0)
    {
        fprintf(stderr, "if_move_to_pid_ns failed: %s\n", strerror(-ret));
    }

    close(fd);
    return ret;
}

/*
 * ============================================================
 * PART 5: SET INTERFACE UP/DOWN
 * ============================================================
 */

int if_set_flags(const char *ifname, unsigned int flags_set, unsigned int flags_clear)
{
    int fd = nl_open();
    if (fd < 0) return -1;

    unsigned int ifindex = if_nametoindex(ifname);
    if (ifindex == 0)
    {
        fprintf(stderr, "Interface %s not found\n", ifname);
        close(fd);
        return -ENODEV;
    }

    char buf[4096];
    memset(buf, 0, sizeof(buf));

    struct nl_msg msg = {
        .buf = buf,
        .size = sizeof(buf),
        .nlh = (struct nlmsghdr *)buf
    };

    msg.nlh->nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
    msg.nlh->nlmsg_type = RTM_NEWLINK;
    msg.nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    msg.nlh->nlmsg_seq = 1;

    struct ifinfomsg *ifi = NLMSG_DATA(msg.nlh);
    ifi->ifi_family = AF_UNSPEC;
    ifi->ifi_index = ifindex;
    ifi->ifi_flags = flags_set;           // flags to set
    ifi->ifi_change = flags_set | flags_clear;  // mask of flags we're changing

    int ret = nl_send_and_wait(fd, msg.nlh);
    close(fd);
    return ret;
}

int if_up(const char *ifname)
{
    return if_set_flags(ifname, IFF_UP, 0);
}

int if_down(const char *ifname)
{
    return if_set_flags(ifname, 0, IFF_UP);
}

/*
 * ============================================================
 * PART 6: ASSIGN IP ADDRESS
 * ============================================================
 */

int if_add_addr(const char *ifname, const char *ip_cidr)
{
    // Parse "192.168.1.1/24" format
    char ip_copy[64];
    strncpy(ip_copy, ip_cidr, sizeof(ip_copy) - 1);

    char *slash = strchr(ip_copy, '/');
    if (!slash)
    {
        fprintf(stderr, "Invalid IP format, expected x.x.x.x/prefix\n");
        return -EINVAL;
    }
    *slash = '\0';
    int prefix_len = atoi(slash + 1);

    struct in_addr addr;
    if (inet_pton(AF_INET, ip_copy, &addr) != 1)
    {
        fprintf(stderr, "Invalid IP address: %s\n", ip_copy);
        return -EINVAL;
    }

    int fd = nl_open();
    if (fd < 0) return -1;

    unsigned int ifindex = if_nametoindex(ifname);
    if (ifindex == 0)
    {
        fprintf(stderr, "Interface %s not found\n", ifname);
        close(fd);
        return -ENODEV;
    }

    char buf[4096];
    memset(buf, 0, sizeof(buf));

    struct nl_msg msg = {
        .buf = buf,
        .size = sizeof(buf),
        .nlh = (struct nlmsghdr *)buf
    };

    // For addresses, we use RTM_NEWADDR and ifaddrmsg instead of ifinfomsg
    msg.nlh->nlmsg_len = NLMSG_LENGTH(sizeof(struct ifaddrmsg));
    msg.nlh->nlmsg_type = RTM_NEWADDR;
    msg.nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_EXCL | NLM_F_ACK;
    msg.nlh->nlmsg_seq = 1;

    struct ifaddrmsg *ifa = NLMSG_DATA(msg.nlh);
    ifa->ifa_family = AF_INET;
    ifa->ifa_prefixlen = prefix_len;
    ifa->ifa_scope = RT_SCOPE_UNIVERSE;
    ifa->ifa_index = ifindex;

    // IFA_LOCAL = the address on this interface
    nl_attr_put(&msg, IFA_LOCAL, &addr, sizeof(addr));
    // IFA_ADDRESS = for point-to-point, the peer; for broadcast, same as LOCAL
    nl_attr_put(&msg, IFA_ADDRESS, &addr, sizeof(addr));

    int ret = nl_send_and_wait(fd, msg.nlh);
    if (ret < 0)
    {
        fprintf(stderr, "if_add_addr failed: %s\n", strerror(-ret));
    }

    close(fd);
    return ret;
}

/*
 * ============================================================
 * PART 7: PUTTING IT ALL TOGETHER
 * ============================================================
 */

// Example: Full container network setup
int setup_container_network(pid_t container_pid,
                            const char *host_if, const char *host_ip,
                            const char *cont_if, const char *cont_ip)
{
    int ret;

    // Step 1: Create the veth pair (both ends start in our namespace)
    printf("Creating veth pair: %s <-> %s\n", host_if, cont_if);
    ret = veth_create(host_if, cont_if);
    if (ret < 0) return ret;

    // Step 2: Move one end into the container's network namespace
    printf("Moving %s to container (pid %d)\n", cont_if, container_pid);
    ret = if_move_to_pid_ns(cont_if, container_pid);
    if (ret < 0) return ret;

    // Step 3: Configure the host end
    printf("Configuring %s with %s\n", host_if, host_ip);
    ret = if_add_addr(host_if, host_ip);
    if (ret < 0) return ret;

    ret = if_up(host_if);
    if (ret < 0) return ret;

    // Step 4: Container end must be configured FROM INSIDE the container
    // (or via nsenter/setns). That's a separate concern.

    printf("Host side ready. Container must configure %s with %s\n",
           cont_if, cont_ip);

    return 0;
}

/*
 * Demo main
 */
// int main(int argc, char *argv[])
// {
//     if (argc < 2)
//     {
//         // Just create a veth pair in current namespace for testing
//         printf("Usage: %s <container_pid>\n", argv[0]);
//         printf("Running demo: creating veth pair in current namespace\n\n");

//         if (veth_create("veth_host", "veth_cont") < 0)
//         {
//             return 1;
//         }

//         if_add_addr("veth_host", "10.0.0.1/24");
//         if_add_addr("veth_cont", "10.0.0.2/24");
//         if_up("veth_host");
//         if_up("veth_cont");

//         printf("\nSuccess! Verify with:\n");
//         printf("  ip link show type veth\n");
//         printf("  ip addr show veth_host\n");
//         printf("  ping -c 1 10.0.0.2\n");

//         return 0;
//     }

//     pid_t container_pid = atoi(argv[1]);
//     return setup_container_network(
//         container_pid,
//         "veth_host", "10.0.0.1/24",
//         "veth_cont", "10.0.0.2/24"
//     );
// }