/* SPDX-License-Identifier: MIT
 * Question-driven, reversible diagnostics. No settings change implicitly. */
#include "help.h"
#include <stdio.h>
#include <string.h>
struct wizard { const char *topic,*question,*yes,*no,*check; };
static const struct wizard wizards[]={
    {"hung","Does the problem happen in another program?","The problem may affect a shared component. Open Hardware Report and compare the drivers in use.","The problem is limited to this program. Save other work, then close it in Task Manager and start it again.","After restarting the program, can you complete the operation?"},
    {"nosound","Is the master volume muted or at zero?","Open Volume Control. Clear Mute and raise Master and Wave. Check the speaker cable and volume.","Check Wave volume and the output connector. Try a known sound in another program; inspect the audio device in Hardware Report.","After checking the volume and connection, can you hear sound?"},
    {"display-trouble","Did the problem start after changing the display mode?","Open Display settings. Select the previous supported mode and confirm only if the picture is readable.","Check the monitor connection and its controls. Inspect the display adapter in Hardware Report and try a supported lower resolution.","Is the screen readable in the supported mode?"},
    {"dosgame-trouble","Does the program run from the DOS prompt?","Check its working directory, sound configuration and required data files. Compare the settings used when it ran.","Read the game's requirements. Check its files and conventional/extended memory requirements in the DOS session.","Does the game start with the required files and settings?"},
    {"network-trouble","Is the network cable connected and is link detected?","Check the IP address, subnet mask and gateway. Try an address on the local network before testing DNS names.","Connect the cable and check the switch port. Inspect the network device and link status before changing IP settings.","Can you reach the local network now?"},
    {"disk-trouble","Can Files read another file on the same disk?","The original file may be damaged. Preserve it and check a backup. Avoid overwriting the only copy.","Stop unnecessary disk writes. Check free space, the connection and Hardware Report. Back up readable files before a repair.","Can you read the file from a verified backup or another disk?"},
    {"startup-trouble","Did startup or shutdown fail after a configuration change?","Record the change. Restore the last working setting or driver using the available recovery tools. Save other work before restarting.","Disconnect optional devices and record the last boot message. Compare Hardware Report and boot logs with a working startup.","Does the system start and shut down normally after the check?"},
    {"memory-trouble","Does closing unused programs allow the operation to finish?","Memory pressure may be the cause. Save your work, close unused programs and retry the operation in one program at a time.","Record the exact error, program and operation. Check the program's memory requirements and the physical memory reported by Hardware Report.","Can the operation complete with enough available memory?"},
    {"print-trouble","Can the printer print its own test page?","Check its connection, selected port and paper. Try a short document in another program and check the printer's status.","Check power, paper and ink or toner. Follow the printer's manual to clear an error before retrying from the computer.","Can a short document be printed now?"},
    {"hardware-trouble","Did the problem begin after adding or changing a device?","Record the new device and driver. Compare the resources in Hardware Report. Restore the last working configuration if needed; turn power off before moving internal hardware.","Inspect Hardware Report for missing devices or resource conflicts. Compare the device requirements and supported driver with the hardware documentation.","Does the device work after correcting its configuration?"},
    {"graphics-trouble","Does the problem affect more than one Windows game?","Check the display adapter and supported graphics API in Hardware Report and the game's requirements. Try a supported display mode.","Check this game's configuration, data files and required API. Record the exact failure and test another title with the same requirements.","Does the game run with a supported mode and graphics API?"}
};
const char *help_wizard_topic(unsigned kind) { return kind<sizeof(wizards)/sizeof(*wizards) ? wizards[kind].topic : NULL; }
static void wizard_visible(struct help_app *a,int on)
{
    const int ids[]={H_WIZ_BACK,H_WIZ_NEXT,H_WIZ_CANCEL,H_WIZ_YES,H_WIZ_NO};
    for (unsigned i=0;i<sizeof(ids)/sizeof(*ids);i++) { struct du_widget *w=du_get(a->ui,ids[i]); w->hidden=!on; du_invalidate(a->ui,w->box); }
}
static void wizard_show(struct help_app *a)
{
    const struct wizard *w=&wizards[a->wizard-1]; char text[2048];
    if (a->wizard_step==0) snprintf(text,sizeof(text),"Troubleshooter\n\n%s\n\nChoose Yes or No, then Next. Back revisits your answers. Cancel returns to Help. No settings change automatically.",w->question);
    else if (a->wizard_step==1 || a->wizard_step==2) snprintf(text,sizeof(text),"Troubleshooter\n\n%s\n\n%s",a->wizard_step==1 ? w->yes : w->no,w->check);
    else if (a->wizard_step==3) snprintf(text,sizeof(text),"Troubleshooter complete\n\nThe problem is resolved. Keep a note of the change that helped. Choose Finish to return to Help.");
    else snprintf(text,sizeof(text),"Further assistance\n\nThe problem is still present. Record the exact error and the checks you tried. Open Hardware Report to collect device and driver information. Consult the component's documentation. Choose Finish to return to Help.");
    help_load_text(a,text,strlen(text),"Troubleshooter");
    a->wizard_answer=-1;
    int final=a->wizard_step>=3;
    du_get(a->ui,H_WIZ_YES)->hidden=final; du_get(a->ui,H_WIZ_NO)->hidden=final;
    du_get(a->ui,H_WIZ_YES)->value=du_get(a->ui,H_WIZ_NO)->value=0;
    du_get(a->ui,H_WIZ_BACK)->disabled=!a->wizard_depth;
    du_get(a->ui,H_WIZ_NEXT)->disabled=!final;
    du_set_text(du_get(a->ui,H_WIZ_NEXT),final ? "&Finish" : "&Next >");
    help_layout(a); du_invalidate(a->ui,(struct du_rect){0,44,a->ui->canvas.width,a->ui->canvas.height-44});
    du_focus(a->ui,final ? H_WIZ_NEXT : H_WIZ_YES);
}
void help_wizard_begin(struct help_app *a,int kind)
{
    if (kind<0 || !help_wizard_topic((unsigned)kind)) return;
    a->wizard=kind+1; a->wizard_step=0; a->wizard_depth=0;
    wizard_visible(a,1); wizard_show(a);
}
void help_wizard_action(struct help_app *a,int id)
{
    if (!a->wizard) return;
    if (id==H_WIZ_CANCEL || (id==H_WIZ_NEXT && a->wizard_step>=3)) {
        int topic=help_topic_find(wizards[a->wizard-1].topic);
        a->wizard=0; wizard_visible(a,0); help_show_topic(a,topic); help_layout(a); return;
    }
    if (id==H_WIZ_YES || id==H_WIZ_NO) {
        a->wizard_answer=id==H_WIZ_YES;
        du_get(a->ui,H_WIZ_NEXT)->disabled=0; du_invalidate_widget(a->ui,H_WIZ_NEXT); return;
    }
    if (id==H_WIZ_BACK && a->wizard_depth) { a->wizard_step=(int)a->wizard_history[--a->wizard_depth]; wizard_show(a); }
    if (id==H_WIZ_NEXT && a->wizard_answer>=0 && a->wizard_depth<16) {
        a->wizard_history[a->wizard_depth++]=(unsigned)a->wizard_step;
        a->wizard_step=a->wizard_step==0 ? a->wizard_answer ? 1 : 2 : a->wizard_answer ? 3 : 4;
        wizard_show(a);
    }
}
