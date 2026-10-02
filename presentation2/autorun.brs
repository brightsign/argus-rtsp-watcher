' autorun.brs - Argus RTSP Watcher 8-up video wall launcher.
'
' Boots a full-screen HTML widget that tiles the eight locally exported
' annotated MJPEG feeds (127.0.0.1:20200..20207) into a 4x2 grid.
' All display logic lives in index.html; this launcher only hosts it.

Sub Main()
    videoMode = CreateObject("roVideoMode")
    ' The wall is authored for 1080p. SetMode keeps the panel and the widget
    ' rectangle in lock-step; to retarget, change the mode here AND the rect.
    videoMode.SetMode("1920x1080x60p")

    rect = CreateObject("roRectangle", 0, 0, 1920, 1080)

    config = {
        url: "file:///sd:/index.html",
        mouse_enabled: false
    }

    htmlWidget = CreateObject("roHtmlWidget", rect, config)
    htmlWidget.Show()

    msgPort = CreateObject("roMessagePort")
    htmlWidget.SetPort(msgPort)

    print "argus 8-up video wall started"

    ' Keep the app alive and surface load results on the console so a
    ' silent black screen can be told apart from a load failure.
    while true
        msg = wait(0, msgPort)

        if type(msg) = "roHtmlWidgetEvent" then
            eventData = msg.GetData()

            if eventData.reason = "load-finished" then
                print "video wall loaded: "; eventData.url
            else if eventData.reason = "load-error" then
                print "video wall load error: "; eventData.url
                print "  message: "; eventData.message
            end if
        end if
    end while
End Sub
