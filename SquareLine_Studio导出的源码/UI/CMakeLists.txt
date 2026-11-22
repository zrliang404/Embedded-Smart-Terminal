SET(SOURCES screens/ui_Main.c
    screens/ui_Music.c
    screens/ui_MonitorPage.c
    screens/ui_AIPage.c
    screens/ui_SettingsPage.c
    screens/ui_Weather_Page.c
    ui.c
    components/ui_comp_hook.c
    ui_helpers.c
    images/ui_img_1070165196.c
    images/ui_img_344699915.c
    images/ui_img_365753981.c
    images/ui_img_240510328.c
    images/ui_img_808252844.c
    images/ui_img_1887403499.c
    images/ui_img_843332074.c
    images/ui_img_944254084.c
    images/ui_img_1680245803.c
    images/ui_img_387312750.c
    images/ui_img_408242217.c
    images/ui_img_267756134.c
    images/ui_img_933793681.c
    images/ui_img_853450402.c
    images/ui_img_862951715.c
    images/ui_img_417419544.c
    images/ui_img_745566655.c
    images/ui_img_1298347426.c
    ui_temporary_image.c
    fonts/ui_font_monitorPage.c
    fonts/ui_font_Time.c
    fonts/ui_font_Weather.c
    fonts/ui_font_Week.c)

add_library(ui ${SOURCES})
