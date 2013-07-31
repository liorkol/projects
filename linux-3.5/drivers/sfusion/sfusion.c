#include "sfusion.h"

//struct iphdr *ip_hdr;

module_init(device_init);
module_exit(device_exit);

/*
 * A struct that holds a device name
 * and a link to the list header.
 * amount of sends through this second
 * amount of sends last seconde
 */
struct device_list {
	struct net_device *dev;
	struct list_head list;

	int amount_sent;
	int last_sec_amount_sent;
};
/*
 * A struct that holds a rule
 * a link to the list header.
 *
 * A rule consists of:
 * - An array of ports that are part of the saiyan fusion.
 * - The type of the ports. Can be "src" or "dest".
 * - The subnet that will be included. e.g. "192.168.0.1/16".
 * - The subnet's type. Can be "src" or "dest".
 * - The protocol. Can be "tcp" or "udp".
 */
struct rule_list {
	int id;
	int *ports;
	char *port_type;
	char *subnet;
	char *subnet_type;
	char *protocol;
	struct list_head list;
};
/*
 * Struct that maps the char device's file operations
 * to their implementation in the code.
 */
static struct file_operations fops = {
	.read = device_read,
	.write = device_write,
	.open = device_open,
	.release = device_release
};
/*
 * struct that holds the timer
*/
static struct timer_list bandwidth_timer;

/* Will be initialized with device's major */
static int device_major = -1;
/* Holds the amount of devices that were opened */
static int device_opened = 0;
/* Holds a list of devices that were set by the user */
static device_list mydevs;
/* Holds a list of rules that were set by the user */
static rule_list myrules;
/* Counter for the rules */
static int num_rules = 0;
static int for_eof = 0;

 
/**
 * device_init		-	Initialize the char device.
 */ 
int device_init(void) {
	int op_result=0;
	int sec_as_jiffies=usecs_to_jiffies(1);

	// Set a new available device major.
	device_major = register_chrdev(0, DEVICE_NAME, &fops);
	// Device major wasn't set.
	if(device_major < 0) {
		printk(KERN_ALERT "sfusion: cannot obtain major number %d.\n", device_major);
		return device_major;
	}
	printk(KERN_INFO "sfusion: sfusion loaded with major %d.\n", device_major);

	//initialise timer
	setup_timer(&bandwidth_timer, set_bandwith, 0);
	mod_timer(&bandwidth_timer, sec_as_jiffies+jiffies);

	//intialise netfilter hooks
	op_result=init_netfilter_loadbalancer_hook();
	if(op_result!=0)
		return op_result;

	return 0;
}
/**
 * this is the timers callback method
 * //switch devices past second packets counter and reset current
**/
void set_bandwith(unsigned long data){
	struct device_list *device_iter=&mydevs;
	while(device_iter!=NULL){
		device_iter->last_sec_amount_sent=device_iter->amount_sent;
		device_iter->amount_sent=0;
		device_iter=(struct device_list *)device_iter->list.next;
	}
}

/**
 * the post routing hook set to netfilter
**/
static struct nf_hook_ops nfho_post =
{
	.hook = loadbalance_hook,
	.hooknum = NF_INET_POST_ROUTING,
	.pf = NFPROTO_IPV4,
	.priority = NF_IP_PRI_FIRST,
};
/**
 * check if loadbalncing nedded and load balance when needed
**/
unsigned int loadbalance_hook
	(unsigned int hooknum,
	struct sk_buff *skb,
	const struct net_device *in,
	const struct net_device *out,
	int (*okfn)(struct sk_buff *))
{
	struct sk_buff *new_skb=NULL;
	struct net_device *new_device=NULL;

	if(!should_loadbalance(skb))
	{
		update_device_counters(skb);
		return NF_ACCEPT;
	}

	//start load balancing
	printk(KERN_DEBUG "loadbalancing needed");
	new_skb=skb_copy(skb,GFP_ATOMIC);
	if(new_skb==NULL)
	{
		printk(KERN_WARNING "could not copy socket buffer\n");
		goto send_no_loadbalancing;
	}

	new_device=get_new_device();
	if(new_device==NULL)
	{
		printk(KERN_WARNING "could not generate device\n");
		goto free_skb;
	}

	new_skb->dev=new_device;
	dev_queue_xmit(new_skb);
	update_device_counters(new_skb);
	printk(KERN_DEBUG "sending with loadbalancing");
	return NF_DROP;

//on errors send as normal packet
free_skb:
	if(new_skb!=NULL)
		kfree_skb(new_skb);
send_no_loadbalancing:
	printk(KERN_DEBUG "sending without loadbalancing\n");
	update_device_counters(skb);
	return NF_ACCEPT;
}
/*struct nf_hook_ops nfho_post =
{
	.hook = nf_hookfn2,
	.hooknum = NF_INET_POST_ROUTING,
	.pf = NFPROTO_IPV4,
	.priority = NF_IP_PRI_FIRST,
};*/
int init_netfilter_loadbalancer_hook(void)
{
	printk(KERN_INFO "registering  hook");
	return nf_register_hook(&nfho_post);
}
/**
 * thif function return true if the given packet should be load balanced
 * skb is the packet to check
**/
bool should_loadbalance(struct sk_buff *skb)
{
	struct device_list* dev_iter=&mydevs;
	bool dev_exists=false;
	struct rule_list* rule_iter=&myrules;
	struct iphdr *ip_hdr=NULL;
	struct tcphdr* tcp_header = NULL;
	struct udphdr* udp_header = NULL;
	int packet_srcport=0;
	int packet_destport=0;
	char *subnet_pointer=NULL;
	int subnet_part=0;
	unsigned int subnet=0;
	int cidr=0;
	char* str_subnet_part=NULL;
	unsigned int ip=0;

	//check if device should be load balanced
	do
	{
		if(strncmp(skb->dev->name, dev_iter->dev->name, IFNAMSIZ)==0)
			dev_exists=true;
		
		dev_iter=(struct device_list *)dev_iter->list.next;
	}while(dev_iter!=&mydevs && dev_iter!=NULL);

	if(!dev_exists)
		return false;

	//check if the packet meets any rule requirement
	for(;rule_iter!=&myrules && rule_iter!=NULL;rule_iter=(struct rule_list *)rule_iter->list.next)
	{
		ip_hdr=(struct iphdr*)skb_network_header(skb);
		
		//check protocol and port
		if(rule_iter->protocol!=NULL)
		{
			if(ip_hdr->protocol==IPPROTO_UDP
				&& strncmp(rule_iter->protocol,"udp",3)!=0
				&& strncmp(rule_iter->protocol,"UDP",3)!=0)
					continue;
			if(ip_hdr->protocol==IPPROTO_TCP
				&& strncmp(rule_iter->protocol,"tcp",3)!=0
				&& strncmp(rule_iter->protocol,"TCP",3)!=0)
					continue;

			//check port and side
			if(rule_iter->port_type!=NULL && rule_iter->ports!=NULL)
			{
				if(ip_hdr->protocol==IPPROTO_TCP)
				{
					tcp_header = (struct tcphdr*)skb_transport_header(skb);//by internet sometimes incorrect and can use (struct tcphdr*)((char *)ip_hdr+ip_hdr->ihl*4) instead
					packet_srcport=tcp_header->source;
					packet_destport=tcp_header->dest;
				}
				else if(ip_hdr->protocol==IPPROTO_UDP)
				{
					udp_header = (struct udphdr*)skb_transport_header(skb);
					packet_srcport=udp_header->source;
					packet_destport=udp_header->dest;
				}
				else
					continue;

				//check every port exists with correct side
				if(strncmp(rule_iter->port_type,"src",3)==0)
					{}
				else
					{}
			}
		}

		//check subnet
		if(rule_iter->subnet!=NULL)
		{
			//build subnet and cidr
			subnet_pointer=rule_iter->subnet;

    		str_subnet_part = strsep(&subnet_pointer, "./");
			if(kstrtoint(str_subnet_part,10,&subnet_part)!=0)
				continue;
			subnet|=subnet_part<<24;

			str_subnet_part = strsep(&subnet_pointer, "./");
			if(kstrtoint(str_subnet_part,10,&subnet_part)!=0)
				continue;
			subnet|=subnet_part<<16;

			str_subnet_part = strsep(&subnet_pointer, "./");
			if(kstrtoint(str_subnet_part,10,&subnet_part)!=0)
				continue;
			subnet|=subnet_part<<8;

			str_subnet_part = strsep(&subnet_pointer, "./");
			if(kstrtoint(str_subnet_part,10,&subnet_part)!=0)
				continue;
			subnet|=subnet_part;

			str_subnet_part = strsep(&subnet_pointer, "./");
			if(kstrtoint(str_subnet_part,10,&cidr)!=0)
				continue;

			subnet=subnet>>cidr;
			subnet=subnet<<cidr;

			//get ip
			if(strncmp(rule_iter->subnet_type,"src",3)==0)
				ip=ip_hdr->saddr;
			else
				ip=ip_hdr->daddr;
			
			ip>>=cidr;
			ip<<=cidr;
			
			if(ip!=subnet)
				continue;
		}

		return true;
	}

	return false;
}
/**
 * update that a packet went through this device
**/
static void update_device_counters(struct sk_buff *skb){
	struct device_list* dev_iter=&mydevs;
	while(dev_iter!=NULL){
		if(strncmp(skb->dev->name, dev_iter->dev->name, IFNAMSIZ)==0){
			dev_iter->amount_sent++;
			break;
		}
	}
}
/**
 * get the new loadbalanced net device
**/
struct net_device* get_new_device(void)
{
	int random=0;
	struct device_list* dev_iter=&mydevs;
	int sum_packets=0;

	//sum_packets
	while(dev_iter!=NULL){
		sum_packets+=dev_iter->last_sec_amount_sent;
		dev_iter=(struct device_list *)dev_iter->list.next;
	}

	//generate random int
	get_random_bytes(&random,sizeof(random));
	random=random%sum_packets;

	//choose device
	dev_iter=&mydevs;
	sum_packets=0;
	while(dev_iter!=NULL){
		sum_packets+=dev_iter->last_sec_amount_sent;
		if(sum_packets>=random)
			return dev_iter->dev;
	}
	
	printk(KERN_ERR "no device found to loadbalance");
	return NULL;
}

/**
 * device_exit		-	Destroy the char device.
 */
void device_exit(void) {
	// Release the device.
	unregister_chrdev(device_major, DEVICE_NAME);
	del_timer(&bandwidth_timer);
	printk(KERN_INFO "sufsion: sfusion unloaded.\n");
}
/**
 * device_open		-	Open the device.
 * @nd: Information about the file.
 * @fp: The file.
 * Returns: Action's state. 0 - success.
 */
static int device_open(struct inode *nd, struct file *fp) {
	// Device was already opened.
	// Return that it's busy.
	if(device_opened) return -EBUSY;
	// Increase number of devices that were opened.
	device_opened++;
	// Acquire module.
	try_module_get(THIS_MODULE);
	// Success.
	return 0;
}
/**
 * device_release		-	Release the device.
 * @nd: Information about the file.
 * @fp: The file.
 * Returns: Action's state. 0 - success.
 */
static int device_release(struct inode *nd, struct file *fp) {
	// Decrease number of devices that were opened.
	if(device_opened) device_opened--;
	// Release module.
	module_put(THIS_MODULE);
	// Success.
	return 0;
}
/**
 * device_read		-	Read from the device.
 * @fp: The file.
 * @buff: Buffer that will hold all the response.
 * 		  The response consists of 2 parts:
 *		  - All the devices that are part of the "saiyan fusion".
 *		  - All the rules that were added by the user and are active.
 * @length: Size of the buffer.
 * @offset: Where to start writing.
 * Returns: Amount of bytes that were written in the buffer.
 */
static ssize_t device_read(struct file *fp, char *buff, size_t length, loff_t *offset) {
	struct list_head *pos;
	struct device_list *tmp_device_list;
	struct rule_list *tmp_rule_list;
	int i=0, j=0, k=0;
	char msg[BUFFER_SIZE];
	
	// Change to EOF and don't run forever.
	for_eof = (for_eof + 1) % 2;	
	// Check if devices list isn't empty.
	if(list_empty(&mydevs.list) == 0)
	{
		j = 0;
		// Add text to the response.
		strcat(msg, "set_devices ");
		// Run on the list.
		list_for_each(pos, &mydevs.list) {
			j++;
			// Get node.
			tmp_device_list = list_entry(pos, struct device_list, list);
			// Add device to the response.
			sprintf(msg, "%s%s", msg, tmp_device_list->dev->name);
		}
		strcat(msg, "\n");
	}
	pos = NULL;
	// Check if rules list isn't empty.
	if(!list_empty(&myrules.list))
	{
		k = 0;
		// Add text to the response.
		strcat(msg, "add_rule ");
		// Run on the list.
		list_for_each(pos, &myrules.list) {
			k++;
			// Get node.
			tmp_rule_list = list_entry(pos, struct rule_list, list);
			// Add text to the response.
			strcat(msg, "-ports ");
			// Run on the ports.
			// Array size is at first element.
			for(i = 1; i <= (tmp_rule_list->ports)[0]; i++)
			{
				// Add port to the response.
				sprintf(msg, "%s%d ", msg, (tmp_rule_list->ports)[i]);
			
			}
			// Add the rest of the elements to the response.
			sprintf(msg, "%s -ports_type %s ", msg, tmp_rule_list->port_type);
			sprintf(msg, "%s -subnet_type %s ", msg, tmp_rule_list->subnet_type);
			sprintf(msg, "%s -protocol %s\n", msg, tmp_rule_list->protocol);
		}
	}
	printk(KERN_DEBUG "sfusion: devs %d , rules %d\n", j, k);
	// If it was requested twice, return 0.
	if(for_eof == 0)
		return 0;
	// Write to buffer.
	copy_to_user(buff, msg, strlen(msg));
	// Send amount of bytes that were written.
	return strlen(msg);
}
/**
 * device_write		-	Write to the device.
 * @fp: The file.
 * @buff: Buffer that holds the request passed from the user.
 *		  The request can be one of the following:
 *		  - set_devices eth0,eth1,...
 *		  - add_rule -ports 80,443,445 -port_type src -subnet 192.168.0.0/16
 *			-subnet_type dest -protocol tcp
 *		  - remove_rule 1
 * @length: The amount of bytes that were written to the buffer.
 * @offset: From where should the buffer be read.
 * Returns: Amount of bytes that were read from the buffer.
 */
static ssize_t device_write(struct file *fp, const char *buff, size_t length, loff_t *offset) {
	char *word;
	char *val;
	int word_length = 0;
	int buff_offset = 0;
	int buff_length = length;
	int val_length = 0;
	int word_offset = 0;
	int num_ports = 0;
	int count = 0;
	long rule_to_del = 0;
	int *ports;
	struct net *initnet = &init_net;
	struct net_device *curr_dev;
	struct device_list *tmp_device_list;
	struct rule_list *tmp_rule_list;
	struct list_head *pos;
	// Initialize a list that will hold the devices.
	INIT_LIST_HEAD(&mydevs.list);
	// Initialize a list that will hold the rules.
	INIT_LIST_HEAD(&myrules.list);
	// Allocate space for the words.
	word = kmalloc(length * sizeof(char), GFP_KERNEL);
	// Get the first word.
	word_length = get_next_word(buff + buff_offset, word, &buff_length, &buff_offset, WORD_SEPARATOR);
	// There is only one word.
	if(word_length == buff_length)
		goto too_short_failed;
	// Check if word equals to set_devices.
	if(strmatch(word, "set_devices"))
	{
		// Move to the next word.
		word_length = get_next_word(buff + buff_offset, word, &buff_length, &buff_offset, NEWLINE_SEPARATOR);
		// This isn't the last word.
		if(buff_length != 0)
			goto unknown_word_failed;
		// Allocate space for all the values in the word.
		val =  kmalloc(word_length * sizeof(char), GFP_KERNEL);
		// Run until the last value.
		do
		{
			// Move to the first/next value.
			val_length = get_next_word(word + word_offset, val, &word_length, &word_offset, VALUE_SEPARATOR);
			// Get the device of the specified name.
			curr_dev = dev_get_by_name(initnet, val);
			if(curr_dev == NULL)
				goto dev_null_failed;
			// Allocate a node in the list.
			tmp_device_list = kmalloc(sizeof(struct device_list), GFP_KERNEL);
			// Add device to the node.
			tmp_device_list->dev = curr_dev;
			tmp_device_list->amount_sent=0;
			tmp_device_list->last_sec_amount_sent=0;
			// Add node to the list.
			list_add(&(tmp_device_list->list), &(mydevs.list));
		} while(word_length > 0);
		// Free allocated space for the values.
		kfree(val);
	}
	// Check if word equals to add_rule.
	else if(strmatch(word, "add_rule"))
	{
		// Allocate a node in the list.
		tmp_rule_list = kmalloc(sizeof(struct rule_list), GFP_KERNEL);
		// Run until the last value.
		do
		{
			// Move to the next word.
			word_length = get_next_word(buff + buff_offset, word, &buff_length, &buff_offset, WORD_SEPARATOR);
			// Check if word equals to -ports.
			if(strmatch(word, "-ports"))
			{
				printk(KERN_INFO "sfusion: text1 %s.\n", word);
				// Move to the next word.
				word_length = get_next_word(buff + buff_offset, word, &buff_length, &buff_offset, WORD_SEPARATOR);
				// Count the number of ports.
				num_ports = strcount(word, word_length, VALUE_SEPARATOR);
				// Initialize port array.
				ports = kmalloc((num_ports + 1) * sizeof(int), GFP_KERNEL);
				// Save size in the first cell.
				ports[0] = num_ports;
				// Allocate space for all the values in the word.
				val =  kmalloc(word_length * sizeof(char), GFP_KERNEL);
				count = 1;
				// Run until the last value.
				do
				{
					// Move to the first/next value.
					val_length = get_next_word(word + word_offset, val, &word_length, &word_offset, VALUE_SEPARATOR);
					// Convert string to int
					// and save in the array.
					if(kstrtoint(val, 10, &(ports[count])) != 0)
						goto port_not_int_failed;
					count++;
				} while(word_length > 0);
				printk(KERN_INFO "portsss: %d\n", ports[0]);
				// Save port array.
				tmp_rule_list->ports = ports;
				// Free allocated space for the values.
				kfree(val);
			}
			// Check if word equals to -port_type.
			else if(strmatch(word, "-port_type"))
			{
				// Move to the next word.
				word_length = get_next_word(buff + buff_offset, word, &buff_length, &buff_offset, WORD_SEPARATOR);
				// Check if word equals to src or dest.
				if(strmatch(word, "src") || strmatch(word, "dest"))
				{
					// Set port type.
					tmp_rule_list->port_type = strclone(word);
					printk(KERN_INFO "sfusion: text2 %s.\n", word);
				}
				// Unknown word.
				else
					goto unknown_word_failed;
			}
			// Check if word equals to -subnet.
			else if(strmatch(word, "-subnet"))
			{
				printk(KERN_INFO "sfusion: text3 %s.\n", word);
			}
			// Check if word equals to -subnet_type.
			else if(strmatch(word, "-subnet_type"))
			{
				// Move to the next word.
				word_length = get_next_word(buff + buff_offset, word, &buff_length, &buff_offset, WORD_SEPARATOR);
				// Check if word equals to src or dest.
				if(strmatch(word, "src") || strmatch(word, "dest"))
				{
					// Set subnet type.
					tmp_rule_list->subnet_type = strclone(word);
					printk(KERN_INFO "sfusion: text4 %s.\n", word);
				}
				// Unknown word.
				else
					goto unknown_word_failed;
			}
			// Check if word equals to -protocol.
			else if(strmatch(word, "-protocol"))
			{
				// Move to the next word.
				word_length = get_next_word(buff + buff_offset, word, &buff_length, &buff_offset, WORD_SEPARATOR);
				// Check if word equals to tcp or udp.
				if(strmatch(word, "tcp") || strmatch(word, "udp"))
				{
					// Set subnet type.
					tmp_rule_list->protocol = strclone(word);
					printk(KERN_INFO "sfusion: text5 %s.\n", word);
				}
				// Unknown word.
				else
					goto unknown_word_failed;
			}
		} while(buff_length > 0);
		// Check that all elements were filled.
		if(tmp_rule_list->ports != NULL
		   && tmp_rule_list->port_type != NULL
		   && tmp_rule_list->subnet != NULL
		   && tmp_rule_list->subnet_type != NULL
		   && tmp_rule_list->protocol != NULL)
		{
			// Insert id.
			tmp_rule_list->id = num_rules;
			num_rules++;
			// Add node to the list.
			list_add(&(tmp_rule_list->list), &(myrules.list));			
		}
	}
	// Check if word equals to remove_rule.
	else if(strmatch(word, "remove_rule"))
	{
		// Move to the next word.
		word_length = get_next_word(buff + buff_offset, word, &buff_length, &buff_offset, NEWLINE_SEPARATOR);
		// This isn't the last word.
		if(buff_length != 0)
			goto unknown_word_failed;
		// Convert string to int.
		if(kstrtol(word, 10, &rule_to_del) != 0)
			goto rule_not_int_failed;
		// Go over the rules.
		list_for_each(pos, &myrules.list) {
			tmp_rule_list = list_entry(pos, struct rule_list, list);
			// Found rule.
			if(tmp_rule_list->id == rule_to_del)
				// Delete it.
				list_del(&tmp_rule_list->list);
		}
	}
	else
		goto unknown_word_failed;
	goto success;
	port_not_int_failed:
	printk(KERN_ALERT "sfusion: One of the ports wasn't an integer.\n");
	// Free allocated space for the ports.
	kfree(ports);
	// Free allocated space for the values.
	kfree(val);
	// Free allocated space for the words.
	kfree(word);
	return -1;
	rule_not_int_failed:
	printk(KERN_ALERT "sfusion: The rule isn't an integer.\n");
	// Free allocated space for the words.
	kfree(word);
	return -1;
	too_short_failed:
	printk(KERN_ALERT "sfusion: Some parameters are missing in the input.\n");
	// Free allocated space for the words.
	kfree(word);
	return -1;
	unknown_word_failed:
	printk(KERN_ALERT "sfusion: Unknown parameter was entered..\n");
	// Free allocated space for the words.
	kfree(word);
	return -1;
	dev_null_failed:
	printk(KERN_INFO "sfusion: Unknown device was entered.\n");
	// Free allocated space for the values.
	kfree(val);
	// Free allocated space for the words.
	kfree(word);
	return -1;
	success:
	// Free allocated space for the words.
	kfree(word);
	return length;
}

/**
 * strcount				-	Find the number of occurrences of
 *						a certain char.
 * @str: The input string.
 * @length: The string's length.
 * @seperator: The char that we want to find.
 * Returns: Number of occurrences.
 */
static ssize_t strcount(const char *str,const ssize_t length, const int separator)
{
	int i = 1;
	int str_len = length;
	// Find the first occurrence of the separator.
	char *found = strnchr(str, str_len, separator);
	//printk(KERN_INFO "sfusion: found %s len: %d", found, str_len);
	char *new_pos = found;
	const char * old_pos = str;
	// Find all the rest.
	while (new_pos != NULL) {
		// Shorten string according to our current location.
		str_len -= (new_pos - old_pos);
		i++;
		old_pos = new_pos;
		new_pos = strnchr(old_pos + 1, str_len, separator);
	}
	// Return the number of occurrences.
	return i;
}
/**
 * strmatch				-	Returns true if strings match and
 *						false otherwise.
 * @str1: The first string.
 * @str2: The second string.
 * Returns: True if match and False otherwise.
 */
static bool strmatch(const char* str1, const char* str2)
{
	return (strcmp(str1, str2) == 0);
}
/**
 * strclone				-	Clones the string.
 * @str1: The input string.
 * Returns: The cloned string.
 */
static char *strclone(const char *str)
{
	// Don't forget the /0.
	int len = strlen(str) + 1;
	char *res = kmalloc(len * sizeof(char), GFP_KERNEL);
	memcpy(res, str, len);
	return res;
}
/**
 * get_next_word		-	Gets the next word in the buffer
 *					according to the given separator.
 * @input: The buffer that will be traversed.
 * @output: The word that was found.
 * @input_length: The buffer's valid length.
 * @input_offset: The location from which we will start the search.
 * @separator: The char that separates the words.
 * Returns: The size of the word that was found.
 */
static ssize_t get_next_word(const char *input, char *output, int *input_length, int *input_offset, const int separator)
{
	// Find the first occurrence of the separator.
	char *found = strnchr(input, (*input_length), separator);
	// Will hold the word's size.
	int word_size = 0;
	// Nothing was found - probably the last word.
	if(found == NULL)
	{
		// Set the word size to be the whole input's length.
		word_size = (*input_length) - 1;
	}
	else
	{
		// Get word's length.
		word_size = found - input;
	}
	
	//printk(KERN_DEBUG "sfusion: input %s.\n", input);
	//printk(KERN_DEBUG "sfusion: offset %d input_length %d separator %c.\n", word_size, length, separator);
	//printk(KERN_DEBUG "sfusion: found %s.\n", found);
	
	// Copy the word to the output.
	memcpy(output, input, word_size);
	// Add null terminator to the output string.
	output[word_size] = '\0';
	// Shorten input length, excluding the
	// word that we just found.
	(*input_length) -= word_size + 1;
	// Move input's location to start from
	// the end of the word that we found.
	(*input_offset) += word_size + 1;
	// Return the word's size.
	return word_size + 1;
}

MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("Rony Fragin & Shai Fogel");
MODULE_DESCRIPTION("Saiyan Fusion Helper.");
MODULE_SUPPORTED_DEVICE(DEVICE_NAME);
