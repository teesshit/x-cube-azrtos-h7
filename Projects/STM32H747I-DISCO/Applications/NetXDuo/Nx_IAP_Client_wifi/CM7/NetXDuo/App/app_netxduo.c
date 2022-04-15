/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    app_netxduo.c
  * @author  MCD Application Team
  * @brief   NetXDuo applicative file
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2022 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "app_netxduo.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "app_azure_rtos.h"
#include "main.h"
#include "iap_client.h"
#include "nxd_dns.h"
#include "nxd_http_client.h"
#include "nx_ip.h"
#include "msg.h"
#include <stdbool.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */
static TX_THREAD AppMainThread;
static TX_THREAD AppIapThread;

TX_BYTE_POOL *AppBytePool;

static NX_PACKET_POOL AppPacketPool;

static NX_IP IpInstance;

static NX_DHCP DhcpClient;
static TX_SEMAPHORE DhcpSemaphore;

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN PFP */
static VOID App_Main_Thread_Entry(ULONG thread_input);

static VOID App_IAP_Thread_Entry(ULONG thread_input);
static VOID ip_address_change_notify_callback(NX_IP *ip_instance, VOID *ptr);

/* USER CODE END PFP */
/**
  * @brief  Application NetXDuo Initialization.
  * @param memory_ptr: memory pointer
  * @retval int
  */
UINT MX_NetXDuo_Init(VOID *memory_ptr)
{
  UINT ret = NX_SUCCESS;
  TX_BYTE_POOL *byte_pool = (TX_BYTE_POOL*)memory_ptr;

  /* USER CODE BEGIN MX_NetXDuo_MEM_POOL */
  (void)byte_pool;
  /* USER CODE END MX_NetXDuo_MEM_POOL */

  /* USER CODE BEGIN MX_NetXDuo_Init */

  MSG_INFO("Nx_IAP_Client_wifi application started..\n");
  MSG_INFO("# build: %s-%s, %s, %s %s\n",
           "SDIO",
           "RTOS",
           "Network on host",
           __TIME__, __DATE__);

  MSG_INFO("\nTX_TIMER_TICKS_PER_SECOND: %"PRIu32"\n", (uint32_t)TX_TIMER_TICKS_PER_SECOND);
  MSG_INFO("NX_IP_PERIODIC_RATE      : %"PRIu32"\n\n", (uint32_t)NX_IP_PERIODIC_RATE);

  /* Initialize the NetX system.  */
  nx_system_initialize();

  /* Allocate the memory for packet_pool.  */
  {
    const ULONG pool_size = NX_PACKET_POOL_SIZE;
    VOID *pool_start;

    if (tx_byte_allocate(byte_pool, &pool_start, pool_size, TX_NO_WAIT) != TX_SUCCESS)
    {
      return TX_POOL_ERROR;
    }

    /* Create the Packet pool to be used for packet allocation */
    ret = nx_packet_pool_create(&AppPacketPool, "Main Packet Pool", PAYLOAD_SIZE, pool_start, pool_size);

    if (ret != NX_SUCCESS)
    {
      return NX_NOT_ENABLED;
    }
    MSG_DEBUG("nx_packet_pool_create() done\n");
  }

  /* Allocate the memory for Ip_Instance */
  {
    const ULONG stack_size = NETX_IP_THREAD_STACK_SIZE;
    VOID *stack_ptr;
    if (tx_byte_allocate(byte_pool, &stack_ptr, stack_size, TX_NO_WAIT) != TX_SUCCESS)
    {
      return TX_POOL_ERROR;
    }

    /* Create the main NX_IP instance. */
    ret = nx_ip_create(&IpInstance, "NetX IP Instance 0", 0, 0,
                       &AppPacketPool, nx_driver_cypress_whd_entry,
                       stack_ptr, stack_size, NETX_IP_THREAD_PRIORITY);

    if (ret != NX_SUCCESS)
    {
      return NX_NOT_ENABLED;
    }
    MSG_DEBUG("nx_ip_create() done\n");
  }

  {
    ret = nx_ip_address_change_notify(&IpInstance, ip_address_change_notify_callback, NULL);
    if (ret != NX_SUCCESS)
    {
      Error_Handler();
    }
    MSG_DEBUG("nx_ip_address_change_notify() done\n");
  }

  /* Create the DHCP client. */
  ret = nx_dhcp_create(&DhcpClient, &IpInstance, "DHCP Client");
  if (ret != NX_SUCCESS)
  {
    return NX_NOT_ENABLED;
  }
  MSG_DEBUG("nx_dhcp_create() done\n");

  /* Allocate the memory for ARP */
  {
    const ULONG arp_cache_size = ARP_MEMORY_SIZE;
    VOID *arp_cache_memory;

    if (tx_byte_allocate(byte_pool, &arp_cache_memory, arp_cache_size, TX_NO_WAIT) != TX_SUCCESS)
    {
      return TX_POOL_ERROR;
    }

    /* Enable the ARP protocol and provide the ARP cache size for the IP instance */
    ret = nx_arp_enable(&IpInstance, arp_cache_memory, arp_cache_size);

    if (ret != NX_SUCCESS)
    {
      return NX_NOT_ENABLED;
    }
    MSG_DEBUG("nx_arp_enable() done\n");
  }

  /* Enable the ICMP */
  {
    ret = nx_icmp_enable(&IpInstance);

    if (ret != NX_SUCCESS)
    {
      return NX_NOT_ENABLED;
    }
    MSG_DEBUG("nx_icmp_enable() done\n");
  }

  /* Enable the UDP protocol required for DNS/DHCP communication. */
  {
    ret = nx_udp_enable(&IpInstance);

    if (ret != NX_SUCCESS)
    {
      return NX_NOT_ENABLED;
    }
    MSG_DEBUG("nx_udp_enable() done\n");
  }

  /* Enable the TCP protocol. */
  {
    ret = nx_tcp_enable(&IpInstance);

    if (ret != NX_SUCCESS)
    {
      return NX_NOT_ENABLED;
    }
    MSG_DEBUG("nx_tcp_enable() done\n");
  }

  /* Allocate the memory for main thread */
  {
    const ULONG stack_size = MAIN_THREAD_STACK_SIZE;
    VOID *stack_ptr;

    if (tx_byte_allocate(byte_pool, &stack_ptr, stack_size, TX_NO_WAIT) != TX_SUCCESS)
    {
      return TX_POOL_ERROR;
    }

    /* Create the main thread */
    ret = tx_thread_create(&AppMainThread, "App Main thread", App_Main_Thread_Entry,
                           (ULONG)byte_pool, stack_ptr, stack_size,
                           MAIN_THREAD_PRIORITY, MAIN_THREAD_PRIORITY, TX_NO_TIME_SLICE, TX_AUTO_START);

    if (ret != TX_SUCCESS)
    {
      return NX_NOT_ENABLED;
    }
    MSG_DEBUG("tx_thread_create() done\n");
  }

  /* Allocate the memory for IAP client thread. */
  {
    const ULONG stack_size = IAP_THREAD_STACK_SIZE;
    VOID *stack_ptr;

    if (tx_byte_allocate(byte_pool, &stack_ptr, stack_size, TX_NO_WAIT) != TX_SUCCESS)
    {
      return TX_POOL_ERROR;
    }

    /* Create the IAP client thread. */
    ret = tx_thread_create(&AppIapThread, "App IAP Thread", App_IAP_Thread_Entry,
                           (ULONG)byte_pool, stack_ptr, stack_size,
                           IAP_THREAD_PRIORITY, IAP_THREAD_PRIORITY, TX_NO_TIME_SLICE, TX_DONT_START);

    if (ret != TX_SUCCESS)
    {
      return NX_NOT_ENABLED;
    }
    MSG_DEBUG("tx_thread_create() done\n");
  }

  /* set DHCP notification callback  */
  tx_semaphore_create(&DhcpSemaphore, "DHCP Semaphore", 0);
  /* USER CODE END MX_NetXDuo_Init */

  return ret;
}

/* USER CODE BEGIN 1 */
/**
  * @brief  ip address change callback.
  * @param ip_instance: NX_IP instance
  * @param ptr: user data
  * @retval none
  */
static VOID ip_address_change_notify_callback(NX_IP *ip_instance, VOID *ptr)
{
  NX_PARAMETER_NOT_USED(ip_instance);
  NX_PARAMETER_NOT_USED(ptr);

  MSG_DEBUG(">\"%s\"\n", ip_instance->nx_ip_name);

  /* release the semaphore as soon as an IP address is available. */
  tx_semaphore_put(&DhcpSemaphore);
}

/**
  * @brief  Main thread entry.
  * @param thread_input: ULONG user argument used by the thread entry
  * @retval none
  */
static VOID App_Main_Thread_Entry(ULONG thread_input)
{
  UINT ret;

  MSG_DEBUG(">\n");

  /* Start the DHCP client. */
  ret = nx_dhcp_start(&DhcpClient);
  if (ret != NX_SUCCESS)
  {
    Error_Handler();
  }
  MSG_DEBUG("nx_dhcp_start() done\n");

  /* Wait until an IP address is ready. */
  if (tx_semaphore_get(&DhcpSemaphore, DHCP_IP_TRANSITION_TIMEOUT) != TX_SUCCESS)
  {
    MSG_ERROR("ERROR: Cannot connect WiFi interface!\n");
    Error_Handler();
  }

#ifdef ENABLE_IOT_INFO
  {
    const UINT IpIndex = 0;
    CHAR *InterfaceName;
    ULONG IpAddress;
    ULONG NetMask;
    ULONG MtuSize;
    ULONG PhysicalAddresMsw;
    ULONG PhysicalAddressLsw;

    _nx_ip_interface_info_get(&IpInstance, IpIndex, &InterfaceName,
                              &IpAddress, &NetMask,
                              &MtuSize,
                              &PhysicalAddresMsw, &PhysicalAddressLsw);

    MSG_INFO("\nIP: \"%s\", MTU: %"PRIu32"\n", InterfaceName, (uint32_t)MtuSize);
  }
#endif /* ENABLE_IOT_INFO */

  /* Read back IP address and gateway address. */
  {
    ULONG IpAddress;
    ULONG NetMask;

    ret = nx_ip_address_get(&IpInstance, &IpAddress, &NetMask);

    if (ret != TX_SUCCESS)
    {
      Error_Handler();
    }

    MSG_INFO("\n- Network Interface connected: ");
    PRINT_IP_ADDRESS(IpAddress);
    MSG_INFO("\n");

    MSG_INFO("Mask: ");
    PRINT_IP_ADDRESS(NetMask);
  }

  /* The network is correctly initialized, start the IAP server thread. */
  tx_thread_resume(&AppIapThread);

  /* this thread is not needed any more, we relinquish it */
  tx_thread_relinquish();

  return;
}


/**
  * @brief  IAP thread entry.
  * @param thread_input: ULONG user argument used by the thread entry
  * @retval none
  */
static VOID App_IAP_Thread_Entry(ULONG thread_input)
{
  TX_BYTE_POOL *const byte_pool = (TX_BYTE_POOL *) thread_input;

  MSG_DEBUG(">\n");

  /* Share the byte pool for future use. */
  AppBytePool = byte_pool;

  StartWiFiIAP(byte_pool, &IpInstance);

  MSG_DEBUG("<\n");
}

void NetXDuo_DeInit(void)
{
  MSG_DEBUG(">\n");

  /* delete dhcp client */
  {
    const UINT status = nx_dhcp_delete(&DhcpClient);
    MSG_INFO("nx_dhcp_delete() done with 0x%"PRIx32"\n", (uint32_t)status);
  }

  /* delete Main IP Instance */
  {
    const UINT status = nx_ip_delete(&IpInstance);
    MSG_INFO("nx_ip_delete() done with 0x%"PRIx32"\n", (uint32_t)status);
  }

  {
    CHAR *name = TX_NULL;
    ULONG available_bytes = 0;
    ULONG fragments = 0;
    TX_THREAD *first_suspended;
    ULONG suspended_count;
    TX_BYTE_POOL *next_pool;

    tx_byte_pool_info_get(AppBytePool, &name,
                          &available_bytes, &fragments,
                          &first_suspended, &suspended_count, &next_pool);
    MSG_INFO("Pool \"%s\" (%"PRIu32")\n", (name != TX_NULL) ? name : "", (uint32_t)AppBytePool->tx_byte_pool_size);
    MSG_INFO(" - available bytes: %"PRIu32"\n", (uint32_t)available_bytes);
    MSG_INFO(" - fragments      : %"PRIu32"\n", (uint32_t)fragments);
    MSG_INFO(" - suspended count: %"PRIu32"\n", (uint32_t)suspended_count);
  }

  {
    ULONG total_packets = 0;
    ULONG free_packets = 0;
    ULONG empty_pool_requests = 0;
    ULONG empty_pool_suspensions = 0;
    ULONG invalid_packet_releases = 0;

    nx_packet_pool_info_get(&AppPacketPool,
                            &total_packets, &free_packets,
                            &empty_pool_requests, &empty_pool_suspensions,
                            &invalid_packet_releases);
    MSG_INFO("Packet Pool \"%s\" (%"PRIu32")\n",
             AppPacketPool.nx_packet_pool_name,
             (uint32_t)AppPacketPool.nx_packet_pool_payload_size);
    MSG_INFO(" - total packets          : %"PRIu32"\n", (uint32_t)total_packets);
    MSG_INFO(" - free packets           : %"PRIu32"\n", (uint32_t)free_packets);
    MSG_INFO(" - invalid packet releases: %"PRIu32"\n", (uint32_t)invalid_packet_releases);
  }

  MSG_DEBUG("<\n");
}
/* USER CODE END 1 */
