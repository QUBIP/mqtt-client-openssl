#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "modbus_task.h"
#include "nanomodbus.h"
#include "platform.h"
#include "config.h"
#include "utilis.h"

#define MAX_CONN_ATTEMPS 10

unsigned long shared_value = 0;

pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cond = PTHREAD_COND_INITIALIZER;

// This task reads a holding register and sends it to the MQTT task if test_enable is false,
// otherwise, it generates a random value every second and sends it via MQTT.

void *modbus_task(void *arg)
{
   
    modbus_task_config_t *config = (modbus_task_config_t *)arg;

    if (config == NULL)
    {
        fprintf(stderr, "modbus_task: Error no config!\n");
        pthread_exit(NULL);
        return NULL;
    }
    
    printf("DEBUG: modbus_task - test_enable = %d\n", config->test_enable);

    if (config->test_enable)
    {
        // TEST MODE: Send a random number via MQTT every second
        srand(time(NULL));
        while (1)
        {
            int random_value = (rand() % 1000) + 1;

            printf("modbus_task: TEST MODE - Sending random value: %d\n", random_value);
            
            pthread_mutex_lock(&mutex);
            shared_value = random_value;
            pthread_cond_signal(&cond);
            pthread_mutex_unlock(&mutex);

            sleep_ms(1000); // Wait for 1 second before sending another value
        }
    }
    else
    {
        // NORMAL MODE: Connect to Modbus and read register

        int nAttemps = 0;
        void *conn = NULL;
        do
        {
            conn = connect_tcp(config->plc_ip, config->plc_port);
            if (!conn)
            {
                fprintf(stderr, "modbus_task: [%d] Error connecting to server retry...\n", nAttemps++);
            }
            else
            {
                break;
            }
        } while (nAttemps < MAX_CONN_ATTEMPS);

        if (!conn)
        {
            fprintf(stderr, "modbus_task: Error connecting to server\n");
            return NULL;
        }

        nmbs_platform_conf platform_conf;
        platform_conf.transport = NMBS_TRANSPORT_TCP;
        platform_conf.read = read_fd_linux;
        platform_conf.write = write_fd_linux;
        platform_conf.arg = conn; // Passing our TCP connection handle to the read/write functions

        nmbs_t nmbs;
        nmbs_error err = nmbs_client_create(&nmbs, &platform_conf);
        if (err != NMBS_ERROR_NONE)
        {
            fprintf(stderr, "modbus_task: Error creating modbus client\n");
            if (!nmbs_error_is_exception(err))
            {
                return NULL;
            }
        }

        nmbs_set_read_timeout(&nmbs, 1000);

        do
        {
            uint16_t r_regs[1];
            err = nmbs_read_holding_registers(&nmbs, config->plc_register, 1, r_regs);
            if (err != NMBS_ERROR_NONE)
            {
                fprintf(stderr, "modbus_task: Error reading holding register at address %d - %s\n", 
                        config->plc_register, nmbs_strerror(err));
                if (!nmbs_error_is_exception(err))
                {
                    disconnect(conn);
                    continue;
                }
            }

            printf("modbus_task: Register at address %d: %d\n", config->plc_register, r_regs[0]);

            pthread_mutex_lock(&mutex);
            shared_value = r_regs[0];
            pthread_cond_signal(&cond);
            pthread_mutex_unlock(&mutex);

            sleep_ms(1000);

        } while (err == NMBS_ERROR_NONE && 1);

        disconnect(conn);
    }

    return NULL;
}

