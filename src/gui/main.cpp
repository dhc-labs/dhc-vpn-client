#include <QApplication>
#include "main_window.h"

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName("dhc-vpn");
    QApplication::setOrganizationName("dhc-vpn");

    MainWindow window;
    window.show();
    return QApplication::exec();
}
