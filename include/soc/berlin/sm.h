/********************************************************************************
 * Marvell GPL License Option
 *
 * If you received this File from Marvell, you may opt to use, redistribute and/or
 * modify this File in accordance with the terms and conditions of the General
 * Public License Version 2, June 1991 (the "GPL License"), a copy of which is
 * available along with the File in the license.txt file or by writing to the Free
 * Software Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 or
 * on the worldwide web at http://www.gnu.org/licenses/gpl.txt.
 *
 * THE FILE IS DISTRIBUTED AS-IS, WITHOUT WARRANTY OF ANY KIND, AND THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE ARE EXPRESSLY
 * DISCLAIMED.  The GPL License provides additional details about this warranty
 * disclaimer.
 ********************************************************************************/
#ifndef _SM_H_
#define _SM_H_

#include <linux/list.h>

#define MV_SM_POWER_WARMDOWN_REQUEST		1
#define MV_SM_POWER_WARMDOWN_REQUEST_2		2
#define MV_SM_POWER_WAKEUP_SOURCE_REQUEST	3
#define MV_SM_POWER_WARMDOWN_TIME		11
#define MV_SM_POWER_SYS_RESET			0xFF
#define MV_SM_IR_Linuxready			30
#define MV_SM_TEMP_SAMPLE			0xF0
#define MV_SM_GET_SUSPEND_RESUME_TIME	0xF2

typedef enum
{
	MV_SM_WAKEUP_SOURCE_INVALID = -1,
	MV_SM_WAKEUP_SOURCE_IR = 0,
	MV_SM_WAKEUP_SOURCE_WIFI,
	MV_SM_WAKEUP_SOURCE_WOL,
	MV_SM_WAKEUP_SOURCE_VGA,
	MV_SM_WAKEUP_SOURCE_CEC,
	MV_SM_WAKEUP_SOURCE_TIMER,
	MV_SM_WAKEUP_SOURCE_BUTTON,
	MV_SM_WAKEUP_SOURCE_BT,
	MV_SM_WAKEUP_SOURCE_NUM,
} MV_SM_WAKEUP_SOURCE_TYPE;

typedef enum {
    MV_SM_ID_SYS = 1,
    MV_SM_ID_COMM,
    MV_SM_ID_IR,
    MV_SM_ID_KEY,
    MV_SM_ID_POWER,
    MV_SM_ID_WD,
    MV_SM_ID_TEMP,
    MV_SM_ID_VFD,
    MV_SM_ID_SPI,
    MV_SM_ID_I2C,
    MV_SM_ID_UART,
    MV_SM_ID_CEC,
    MV_SM_ID_WOL,
    MV_SM_ID_LED,
    MV_SM_ID_ETH,
    MV_SM_ID_DDR,
    MV_SM_ID_WIFIBT,
    /* add new module id here */
    MV_SM_ID_DEBUG,
    MV_SM_ID_CONSOLE,
    MV_SM_ID_PMIC,
    MV_SM_ID_AUDIO,
    MV_SM_ID_MAX,
} MV_SM_MODULE_ID;

#define MAX_MSG_TYPE	(MV_SM_ID_MAX - 1)

/* SM driver ioctl cmd */
#define SM_READ			0x1
#define SM_WRITE		0x2
#define SM_RDWR			0x3
#define SM_WAIT_WAKEUP		0x4
#define SM_BT_INFO		0x5
#define SM_Enable_WaitQueue	0x1234
#define SM_Disable_WaitQueue	0x3456

struct sm_ir_handler{
	struct list_head node;
	void (*fn)(int ir_key);
	/* monitored key code, 0 means all key */
	int key;
};

extern int bsm_msg_recv(int id, void *msg, int *len);
extern int bsm_msg_send(int id, void *msg, int len);
extern void register_sm_ir_handler(struct sm_ir_handler *handler);
extern void unregister_sm_ir_handler(struct sm_ir_handler *handler);

#endif /* _SM_H_ */
