#include <dryos.h>
#include <property.h>
#include <cfn-generic.h>

/* HTP: on the 6D this is a dedicated property (not a CFn), so read with
 * GENERIC_GET_HTP and write it back via prop_request_change. */
GENERIC_GET_HTP
void set_htp(int value)
{
    value = COERCE(value, 0, 1);
    prop_request_change(PROP_HTP, &value, 4);
}

/* ALO: also a dedicated property on the 6D. PROP_ALO carries several bytes:
 *   buf[0] actual ALO level (0=std,1=low,2=high,3=off; may be forced off by M mode/HTP)
 *   buf[1] original ALO level
 *   buf[2] 1 = disable ALO in Manual mode, 0 = keep ALO active in M
 * We snapshot the real property buffer (so we use the exact length the camera
 * uses) and only tweak the bytes we care about when writing back. */
static int8_t alo_buf[8];
static int alo_len = 0;
PROP_HANDLER(PROP_ALO)
{
    alo_len = MIN((int)len, (int)sizeof(alo_buf));
    memcpy(alo_buf, buf, alo_len);
}
int get_alo() { return alo_buf[0] & 0xFF; }
void set_alo(int value)
{
    if (!alo_len) return;                 /* wait until we've seen the property once */
    alo_buf[0] = COERCE(value, 0, 3);     /* ALO level */
    if (alo_len > 2) alo_buf[2] = 0;      /* keep ALO working in Manual mode */
    prop_request_change(PROP_ALO, alo_buf, alo_len);
}

GENERIC_GET_MLU
GENERIC_SET_MLU

// POS 8 (shutter): 0=AF-ON, 1=METER, 2=*
// POS 10 (af on): 0=AF-ON, 1=AEL+FEL, 2=AF-OFF, 5=*H, 8=*, 3=FEL, 7=OFF
// POS 12 (ae lock): 0=AEL+FEL, 1=AF-ON, 2=AF-OFF, 5=*H, 8=*, 3=FEL, 7=OFF
// POS 14 (dof preview): 0=DOF, 1=AF-OFF, 2=AEL+FEL, 3=OS-SERVO, 4=IS, 9=LEVEL, 12=*H, 14=*, 8=FEL, 13=OFF
// POS 16 (lens btn): 0=AF-OFF, 1=AF-ON, 2=AEL+FEL, 3=OS-SERVO, 4=IS, 6=*H, 8=*, 9=FEL
// POS 18 (set): 0=OFF, 2=IQ, 4=PS, 6=MENU, 9=ISO, 10=FEC
// POS 20 (main dial): 0=TV, 1=AV
// POS 22 (back dial): 0=AV, 1=TV
// POS 24 (af sel): 0=OFF; 1=AFPT
static int8_t some_cfn[0x1d];
PROP_HANDLER(0x80010007)
{
    ASSERT(len == 0x1d);
    memcpy(some_cfn, buf, 0x1d);
}

int cfn_get_af_button_assignment() { return some_cfn[8]; }
void cfn_set_af_button(int value) 
{  
    some_cfn[8] = COERCE(value, 0, 2);
    prop_request_change(0x80010007, some_cfn, 0x1d);
}
