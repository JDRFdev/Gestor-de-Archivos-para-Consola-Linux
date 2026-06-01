#include <ncurses.h>
#include <cstdlib>
#include <string>
#include <ctime>
#include <dirent.h>
#include <sys/stat.h>
#include <pwd.h>
#include <grp.h>
#include <unistd.h>
#include <vector>
#include <algorithm>
#include <cstring>
#include <sys/wait.h>
#include <pthread.h>
#include <atomic>
#include <sstream>
#include <iomanip>

int TERM_ROWS, TERM_COLS;

WINDOW *win_izq   = nullptr;  //Ventana Izquierda
WINDOW *win_der   = nullptr;  //Ventana Derecha
WINDOW *win_barra = nullptr;  //Ventana Inferior

// ── Enumeración para controlar los modos de la vista derecha ──
enum ModoVista { VISTA_TEXTO, VISTA_HEX, VISTA_PROPS, VISTA_ARBOL };
ModoVista vista_actual = VISTA_TEXTO;

// ── Estructura que representa un archivo o directorio ─────────
struct Entrada {
    std::string nombre;       // Nombre del archivo
    std::string tamanio;      // "1.2 KiB", "3.4 MiB", etc.
    std::string permisos;     // "-rwxr-xr-x"
    std::string fecha;        // "2024-05-01"
    std::string tipo;         // "Dir", "PDF", "Imagen", etc.
    ino_t       inodo;        // Número de i-nodo
    bool        es_dir;       // true si es directorio
    bool        es_ejecutable;
    off_t       tamanio_raw;  // Tamaño en bytes (para ordenar)
};

std::vector<Entrada> entradas_actuales;
int    indice_sel      = 0;   // Entrada seleccionada en el panel izquierdo
int    scroll_offset   = 0;   // Para scroll si hay muchos archivos (izquierdo)

// ── Variables para el control del panel derecho ───────────────
std::vector<std::string> contenido_vista_der; // Líneas de texto/hex a mostrar
int    scroll_der      = 0;   // Desplazamiento vertical en el panel derecho

bool   mostrar_ocultos = false;
bool   mostrar_inodos  = false;
std::string ruta_actual = ".";

std::atomic<bool> hilo_activo(true);  // Controla cuándo para el hilo
pthread_t hilo_reloj;                  // Identificador del hilo
pthread_mutex_t mutex_pantalla = PTHREAD_MUTEX_INITIALIZER;

#define COLOR_SELECCIONADO  1
#define COLOR_DIRECTORIO    2
#define COLOR_EJECUTABLE    3
#define COLOR_NORMAL        4
#define COLOR_BARRA         5
#define COLOR_ESPECIAL      6
#define COLOR_BORDE         7
#define COLOR_TITULO        8


// ── Convierte bytes a notación de ingeniería ──────────────────
std::string formatear_tamanio(off_t bytes) {
    if (bytes < 1024)
        return std::to_string(bytes) + " B";
    else if (bytes < 1024 * 1024)
        return std::to_string(bytes / 1024) + " KiB";
    else if (bytes < 1024 * 1024 * 1024)
        return std::to_string(bytes / (1024 * 1024)) + " MiB";
    else
        return std::to_string(bytes / (1024 * 1024 * 1024)) + " GiB";
}

// ── Convierte el modo stat a string tipo "-rwxr-xr-x" ─────────
std::string formatear_permisos(mode_t mode) {
    std::string p = "----------";
    if (S_ISDIR(mode))  p[0] = 'd';
    if (S_ISLNK(mode))  p[0] = 'l';
    if (mode & S_IRUSR) p[1] = 'r';
    if (mode & S_IWUSR) p[2] = 'w';
    if (mode & S_IXUSR) p[3] = 'x';
    if (mode & S_IRGRP) p[4] = 'r';
    if (mode & S_IWGRP) p[5] = 'w';
    if (mode & S_IXGRP) p[6] = 'x';
    if (mode & S_IROTH) p[7] = 'r';
    if (mode & S_IWOTH) p[8] = 'w';
    if (mode & S_IXOTH) p[9] = 'x';
    return p;
}

// ── Detecta el tipo de archivo por magic number o extensión ───
std::string detectar_tipo(const std::string &ruta, mode_t mode) {
    if (S_ISDIR(mode))  return "Dir";
    if (S_ISLNK(mode))  return "Enlace";

    // Intentar leer magic number (primeros 4 bytes)
    FILE *f = fopen(ruta.c_str(), "rb");
    if (f) {
        unsigned char magic[4] = {0};
        fread(magic, 1, 4, f);
        fclose(f);

        if (magic[0] == 0x25 && magic[1] == 0x50) return "PDF";
        if (magic[0] == 0x89 && magic[1] == 0x50) return "PNG";
        if (magic[0] == 0x47 && magic[1] == 0x49) return "GIF";
        if (magic[0] == 0xFF && magic[1] == 0xD8) return "JPG";
        if (magic[0] == 0x7F && magic[1] == 0x45) return "ELF";
        if (magic[0] == 0x23 && magic[1] == 0x21) return "Script";
    }

    // Fallback por extensión
    auto pos = ruta.rfind('.');
    if (pos != std::string::npos) {
        std::string ext = ruta.substr(pos + 1);
        if (ext == "cpp" || ext == "c" || ext == "h") return "Codigo";
        if (ext == "txt" || ext == "md")              return "Texto";
        if (ext == "zip" || ext == "tar" || ext == "gz") return "Comprimido";
        if (ext == "mp3" || ext == "wav")             return "Audio";
    }

    return "Binario";
}
// ── Lee el directorio y retorna vector de Entradas ────────────
std::vector<Entrada> leer_directorio(const std::string &ruta, bool mostrar_ocultos) {
    std::vector<Entrada> entradas;

    DIR *dir = opendir(ruta.c_str());
    if (!dir) return entradas;

    struct dirent *dp;
    while ((dp = readdir(dir)) != nullptr) {
        std::string nombre = dp->d_name;

        // Saltar "." pero conservar ".."
        if (nombre == ".") continue;

        // Saltar ocultos si el toggle está desactivado
        if (!mostrar_ocultos && nombre[0] == '.' && nombre != "..")
            continue;

        // Obtener info del archivo con stat
        std::string ruta_completa = ruta + "/" + nombre;
        struct stat st;
        if (lstat(ruta_completa.c_str(), &st) != 0) continue;

        Entrada e;
        e.nombre        = nombre;
        e.inodo         = dp->d_ino;
        e.es_dir        = S_ISDIR(st.st_mode);
        e.es_ejecutable = (st.st_mode & S_IXUSR) && !e.es_dir;
        e.tamanio_raw   = st.st_size;
        e.tamanio       = e.es_dir ? "<DIR>" : formatear_tamanio(st.st_size);
        e.permisos      = formatear_permisos(st.st_mode);
        e.tipo          = detectar_tipo(ruta_completa, st.st_mode);

        // Fecha de última modificación
        char buf[16];
        strftime(buf, sizeof(buf), "%Y-%m-%d", localtime(&st.st_mtime));
        e.fecha = buf;

        entradas.push_back(e);
    }
    closedir(dir);

    // Directorios primero, luego archivos, ambos alfabéticos
    std::sort(entradas.begin(), entradas.end(), [](const Entrada &a, const Entrada &b) {
        if (a.nombre == "..") return true;
        if (b.nombre == "..") return false;
        if (a.es_dir != b.es_dir) return a.es_dir > b.es_dir;
        return a.nombre < b.nombre;
    });

    return entradas;
}

//  init_terminal()
//  Pone ncurses en modo raw, activa colores y guarda las dimensiones del terminal en TERM_ROWS / TERM_COLS.
void init_terminal() {
    if (initscr() == nullptr) {
        fprintf(stderr, "Error: terminal no soportado\n");
        exit(1);
    }
    cbreak();              // Teclas sin buffer (sin esperar Enter)
    noecho();              // No imprimir lo que escribe el usuario
    keypad(stdscr, TRUE);  // Habilitar flechas y teclas especiales
    start_color();         // Activar subsistema de colores
    curs_set(0);           // Ocultar el cursor parpadeante
    set_escdelay(25);      // Reducir delay de tecla ESC

    // Obtener tamaño real del terminal
    getmaxyx(stdscr, TERM_ROWS, TERM_COLS);

    // ── Definir pares de color ────────────────────────────────
    init_pair(COLOR_SELECCIONADO, COLOR_BLACK,  COLOR_CYAN);
    init_pair(COLOR_DIRECTORIO,   COLOR_CYAN,   COLOR_BLACK);
    init_pair(COLOR_EJECUTABLE,   COLOR_GREEN,  COLOR_BLACK);
    init_pair(COLOR_NORMAL,       COLOR_WHITE,  COLOR_BLACK);
    init_pair(COLOR_BARRA,        COLOR_BLACK,  COLOR_WHITE);
    init_pair(COLOR_ESPECIAL,     COLOR_YELLOW, COLOR_BLACK);
    init_pair(COLOR_BORDE,        COLOR_BLUE,   COLOR_BLACK);
    init_pair(COLOR_TITULO,       COLOR_WHITE,  COLOR_BLUE);
}

void cleanup_terminal() {
    if (win_izq)   { delwin(win_izq);   win_izq   = nullptr; }
    if (win_der)   { delwin(win_der);   win_der   = nullptr; }
    if (win_barra) { delwin(win_barra); win_barra = nullptr; }
    endwin();
}

void crear_paneles() {
    int filas_content = TERM_ROWS - 3;  // Filas disponibles (sin barra)
    int cols_izq      = TERM_COLS / 2;
    int cols_der      = TERM_COLS - cols_izq;

    // newwin(filas, columnas, fila_inicio, col_inicio)
    win_izq   = newwin(filas_content, cols_izq,      0, 0);
    win_der   = newwin(filas_content, cols_der,      0, cols_izq);
    win_barra = newwin(3,             TERM_COLS,     filas_content, 0);

    // Activar teclas especiales en cada ventana
    keypad(win_izq,   TRUE);
    keypad(win_der,   TRUE);
    keypad(win_barra, TRUE);
}
// ============================================================
//  dibujar_borde_titulo()
//  Dibuja el borde de una ventana y un título en la parte
//  superior, con fondo azul para destacarlo.
// ============================================================
void dibujar_borde_titulo(WINDOW *win, const std::string &titulo) {
    wattron(win,  COLOR_PAIR(COLOR_BORDE));
    box(win, 0, 0);  // Dibuja el borde con caracteres ACS
    wattroff(win, COLOR_PAIR(COLOR_BORDE));

    // Título centrado en la primera línea
    int win_cols = getmaxx(win);
    int pos_x    = (win_cols - (int)titulo.size() - 2) / 2;
    if (pos_x < 1) pos_x = 1;

    wattron(win, COLOR_PAIR(COLOR_TITULO) | A_BOLD);
    mvwprintw(win, 0, pos_x, " %s ", titulo.c_str());
    wattroff(win, COLOR_PAIR(COLOR_TITULO) | A_BOLD);
}
// ============================================================
//  dibujar_panel_izquierdo() -- EXPLORADOR
// ============================================================
void dibujar_panel_izquierdo() {
    werase(win_izq);
    dibujar_borde_titulo(win_izq, "Explorador");

    // Mostrar ruta actual
    wattron(win_izq, COLOR_PAIR(COLOR_ESPECIAL));
    mvwprintw(win_izq, 1, 2, "%.40s", ruta_actual.c_str());
    wattroff(win_izq, COLOR_PAIR(COLOR_ESPECIAL));

    // Separador
    wattron(win_izq, COLOR_PAIR(COLOR_BORDE));
    int cols = getmaxx(win_izq);
    for (int i = 1; i < cols - 1; i++)
        mvwaddch(win_izq, 2, i, ACS_HLINE);
    wattroff(win_izq, COLOR_PAIR(COLOR_BORDE));

    // Encabezado de columnas
    wattron(win_izq, A_BOLD | COLOR_PAIR(COLOR_NORMAL));
    if (mostrar_inodos)
        mvwprintw(win_izq, 3, 2, "%-6s %-18s %7s  %-10s %-10s %-8s",
                  "Inodo","Nombre","Tamaño","Permisos","Fecha","Tipo");
    else
        mvwprintw(win_izq, 3, 2, "%-20s %7s  %-10s %-10s %-8s",
                  "Nombre","Tamaño","Permisos","Fecha","Tipo");
    wattroff(win_izq, A_BOLD | COLOR_PAIR(COLOR_NORMAL));

   // Listar entradas
    int filas_disp = getmaxy(win_izq) - 5;
    for (int i = 0; i < filas_disp && (i + scroll_offset) < (int)entradas_actuales.size(); i++) {
        int idx = i + scroll_offset;
        const Entrada &e = entradas_actuales[idx];
        // Color según tipo
        if (idx == indice_sel)
            wattron(win_izq, COLOR_PAIR(COLOR_SELECCIONADO) | A_BOLD);
        else if (e.es_dir)
            wattron(win_izq, COLOR_PAIR(COLOR_DIRECTORIO));
        else if (e.es_ejecutable)
            wattron(win_izq, COLOR_PAIR(COLOR_EJECUTABLE));
        else
            wattron(win_izq, COLOR_PAIR(COLOR_NORMAL));

        if (mostrar_inodos)
            mvwprintw(win_izq, 4 + i, 2, "%-6lu %-18s %7s  %-10s %-10s %-8s",
                      e.inodo,
                      e.nombre.substr(0,18).c_str(),
                      e.tamanio.c_str(),
                      e.permisos.c_str(),
                      e.fecha.c_str(),
                      e.tipo.c_str());
        else
            mvwprintw(win_izq, 4 + i, 2, "%-20s %7s  %-10s %-10s %-8s",
                      e.nombre.substr(0,20).c_str(),
                      e.tamanio.c_str(),
                      e.permisos.c_str(),
                      e.fecha.c_str(),
                      e.tipo.c_str());

        // Apagar atributos
        wattroff(win_izq, COLOR_PAIR(COLOR_SELECCIONADO) | COLOR_PAIR(COLOR_DIRECTORIO) |
                          COLOR_PAIR(COLOR_EJECUTABLE)   | COLOR_PAIR(COLOR_NORMAL) | A_BOLD);
    }

    wrefresh(win_izq);
}

// ============================================================
//  obtener_usuario() -- Obtiene el nombre del usuario.
// ============================================================
std::string obtener_usuario() {
    struct passwd *pw = getpwuid(getuid());
    return pw ? std::string(pw->pw_name) : "desconocido";
}

// ── Detecta si un archivo es texto ASCII ─────────────────────
bool es_texto(const std::string &ruta) {
    FILE *f = fopen(ruta.c_str(), "rb");
    if (!f) return false;

    unsigned char buf[512];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);

    for (size_t i = 0; i < n; i++) {
        if (buf[i] < 8 || (buf[i] > 13 && buf[i] < 32 && buf[i] != 27))
            return false;
    }
    return true;
}

// ── Genera la vista de texto ASCII ────────────────────────────
void cargar_vista_texto(const std::string &ruta) {
    contenido_vista_der.clear();
    if (!es_texto(ruta)) {
        contenido_vista_der.push_back("No existe vista previa (archivo binario o especial)");
        return;
    }

    FILE *f = fopen(ruta.c_str(), "r");
    if (!f) return;

    char line[512];
    while (fgets(line, sizeof(line), f) && contenido_vista_der.size() < 2000) {
        // Quitar el salto de línea para ncurses
        size_t len = strlen(line);
        if (len > 0 && line[len-1] == '\n') line[len-1] = '\0';
        contenido_vista_der.push_back(line);
    }
    fclose(f);
}

// ── Genera la vista hexadecimal (estilo xxd) ──────────────────
void cargar_vista_hex(const std::string &ruta) {
    contenido_vista_der.clear();
    FILE *f = fopen(ruta.c_str(), "rb");
    if (!f) return;

    unsigned char buffer[16];
    size_t n;
    unsigned int offset = 0;

    while ((n = fread(buffer, 1, 16, f)) > 0 && contenido_vista_der.size() < 2000) {
        std::stringstream ss;
        // Offset: 00000000
        ss << std::hex << std::setw(8) << std::setfill('0') << offset << ": ";

        // Hex bytes: 7f 45 4c 46 ...
        for (size_t i = 0; i < 16; i++) {
            if (i < n)
                ss << std::hex << std::setw(2) << std::setfill('0') << (int)buffer[i] << " ";
            else
                ss << "   ";
            if (i == 7) ss << " "; // Espacio extra en medio
        }

        // ASCII representation
        ss << " ";
        for (size_t i = 0; i < n; i++) {
            if (buffer[i] >= 32 && buffer[i] <= 126)
                ss << (char)buffer[i];
            else
                ss << ".";
        }
        contenido_vista_der.push_back(ss.str());
        offset += 16;
    }
    fclose(f);
}

// ── Genera la vista de árbol recursivamente ───────────────────
void generar_arbol(const std::string &ruta, int nivel, std::string prefijo, bool es_ultimo) {
    if (nivel > 3) return; // Limitar profundidad por rendimiento

    DIR *dir = opendir(ruta.c_str());
    if (!dir) return;

    struct dirent *dp;
    std::vector<std::string> sub;
    while ((dp = readdir(dir)) != nullptr) {
        std::string n = dp->d_name;
        if (n == "." || n == "..") continue;
        if (!mostrar_ocultos && n[0] == '.') continue;
        sub.push_back(n);
    }
    closedir(dir);
    std::sort(sub.begin(), sub.end());

    for (size_t i = 0; i < sub.size(); i++) {
        bool ultimo_hijo = (i == sub.size() - 1);
        std::string conector = ultimo_hijo ? "+-- " : "|-- ";
        contenido_vista_der.push_back(prefijo + conector + sub[i]);

        std::string nueva_ruta = ruta + "/" + sub[i];
        struct stat st;
        if (lstat(nueva_ruta.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            generar_arbol(nueva_ruta, nivel + 1, prefijo + (ultimo_hijo ? "    " : "|   "), ultimo_hijo);
        }
    }
}

// ── Dibuja las propiedades del archivo en win_der ──────────────
void dibujar_propiedades(const std::string &ruta) {
    struct stat st;
    if (lstat(ruta.c_str(), &st) != 0) return;

    struct passwd *pw = getpwuid(st.st_uid);
    struct group  *gr = getgrgid(st.st_gid);

    int y = 3;
    wattron(win_der, COLOR_PAIR(COLOR_NORMAL));
    mvwprintw(win_der, y++, 2, "Propiedades de: %s", ruta.c_str());
    mvwprintw(win_der, y++, 2, "-----------------------------------");
    
    mvwprintw(win_der, y++, 2, "Inodo:        %lu", (unsigned long)st.st_ino);
    mvwprintw(win_der, y++, 2, "Enlaces:      %lu", (unsigned long)st.st_nlink);
    mvwprintw(win_der, y++, 2, "Tamaño:       %ld bytes", (long)st.st_size);
    mvwprintw(win_der, y++, 2, "Permisos (S): %s", formatear_permisos(st.st_mode).c_str());
    mvwprintw(win_der, y++, 2, "Permisos (O): %04o", st.st_mode & 07777);
    mvwprintw(win_der, y++, 2, "Usuario:      %s (%u)", pw ? pw->pw_name : "???", st.st_uid);
    mvwprintw(win_der, y++, 2, "Grupo:        %s (%u)", gr ? gr->gr_name : "???", st.st_gid);

    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", localtime(&st.st_atime));
    mvwprintw(win_der, y++, 2, "Acceso:       %s", buf);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", localtime(&st.st_mtime));
    mvwprintw(win_der, y++, 2, "Modificacion: %s", buf);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", localtime(&st.st_ctime));
    mvwprintw(win_der, y++, 2, "Cambio:       %s", buf);
    
    wattroff(win_der, COLOR_PAIR(COLOR_NORMAL));
}

// ── Carga el contenido necesario según la vista actual ────────
void actualizar_contenido_derecho() {
    if (entradas_actuales.empty()) {
        contenido_vista_der.clear();
        return;
    }

    const Entrada &e = entradas_actuales[indice_sel];
    std::string ruta_completa = ruta_actual + "/" + e.nombre;

    if (vista_actual == VISTA_TEXTO) {
        if (e.es_dir) {
            contenido_vista_der.clear();
            contenido_vista_der.push_back("No existe vista previa para directorios.");
        } else {
            cargar_vista_texto(ruta_completa);
        }
    } else if (vista_actual == VISTA_HEX) {
        if (e.es_dir) {
            contenido_vista_der.clear();
        } else {
            cargar_vista_hex(ruta_completa);
        }
    } else if (vista_actual == VISTA_ARBOL) {
        contenido_vista_der.clear();
        if (e.es_dir && e.nombre != "..") {
            contenido_vista_der.push_back(e.nombre);
            generar_arbol(ruta_completa, 0, "", true);
        } else {
            contenido_vista_der.push_back("Seleccione un directorio para ver el árbol.");
        }
    }
    // VISTA_PROPS se dibuja directamente con stat
}

void dibujar_panel_derecho() {
    werase(win_der);
    
    std::string titulo = "Vista";
    if (vista_actual == VISTA_TEXTO) titulo = "Vista de Texto";
    else if (vista_actual == VISTA_HEX)   titulo = "Vista Hexadecimal";
    else if (vista_actual == VISTA_PROPS) titulo = "Propiedades";
    else if (vista_actual == VISTA_ARBOL) titulo = "Vista de Árbol";
    
    dibujar_borde_titulo(win_der, titulo);

    if (entradas_actuales.empty()) {
        wrefresh(win_der);
        return;
    }

    if (vista_actual == VISTA_PROPS) {
        const Entrada &e = entradas_actuales[indice_sel];
        dibujar_propiedades(ruta_actual + "/" + e.nombre);
    } else {
        int filas_disp = getmaxy(win_der) - 2;
        int cols_disp  = getmaxx(win_der) - 4;

        for (int i = 0; i < filas_disp && (i + scroll_der) < (int)contenido_vista_der.size(); i++) {
            std::string linea = contenido_vista_der[i + scroll_der];
            if ((int)linea.size() > cols_disp) linea = linea.substr(0, cols_disp);
            mvwprintw(win_der, 1 + i, 2, "%s", linea.c_str());
        }
    }

    wrefresh(win_der);
}
// ============================================================
//  dibujar_barra_inferior()
//  Muestra usuario, ruta, atajos y hora actual.
//  El reloj lo manejará un hilo dedicado en la Fase 5.
// ============================================================
void dibujar_barra_inferior(const std::string &usuario) {
    werase(win_barra);
    wbkgd(win_barra, COLOR_PAIR(COLOR_BARRA));

    // Nombre de usuario y atajos
    wattron(win_barra, COLOR_PAIR(COLOR_BARRA) | A_BOLD);
    mvwprintw(win_barra, 0, 1, " %s ", usuario.c_str());
    wattroff(win_barra, A_BOLD);

    mvwprintw(win_barra, 0, 12,"I: Inodos H: Ocultos N: Nano "
        "F2:Texto  F3:Hex  F4:Props  F5:Arbol  "
        "C:Copiar  M:Mover  B:Borrar  R:Renombrar  Q:Salir");
    wattroff(win_barra, COLOR_PAIR(COLOR_BARRA));

    // Fila 1: hora actual (en Fase 5 esto lo actualiza un hilo)
    time_t ahora = time(nullptr);
    char buf_hora[32];
    strftime(buf_hora, sizeof(buf_hora), "%Y-%m-%d  %H:%M:%S", localtime(&ahora));

    wattron(win_barra, COLOR_PAIR(COLOR_BARRA) | A_BOLD);
    int cols = getmaxx(win_barra);
    mvwprintw(win_barra, 1, cols - 22, "%s", buf_hora);
    wattroff(win_barra, COLOR_PAIR(COLOR_BARRA) | A_BOLD);

    wrefresh(win_barra);
}

// ── Abre un archivo en nano como proceso hijo ─────────────────
void abrir_en_nano(const std::string &ruta_archivo) {
    // Suspender ncurses antes de lanzar nano
    def_prog_mode();   // Guarda el modo actual del terminal
    endwin();          // Libera el terminal para que nano lo use

    pid_t pid = fork();

    if (pid == 0) {
        // ── Proceso hijo — se convierte en nano ───────────────
        execlp("nano", "nano", ruta_archivo.c_str(), nullptr);
        perror("execlp nano");
        exit(1);

    } else if (pid > 0) {
        // ── Proceso padre — espera a que nano termine ─────────
        int status;
        waitpid(pid, &status, 0);

    } else {
           perror("fork");
    }

    // Retomar ncurses después de que nano cerró
    reset_prog_mode();  // Restaura el modo guardado
    refresh();          // Redibujar la pantalla
}

void *actualizar_reloj(void *arg) {
    while (hilo_activo) {
        pthread_mutex_lock(&mutex_pantalla);
        if (win_barra) {
            std::string usuario = obtener_usuario();
            dibujar_barra_inferior(usuario);
        }
         pthread_mutex_unlock(&mutex_pantalla);
          napms(100);
    }
    return nullptr;
}
// ============================================================
//  loop_principal() -- Captura teclas
// ============================================================
void loop_principal(const std::string &usuario){
    //Cargar directorio Inicial
     entradas_actuales = leer_directorio(ruta_actual, mostrar_ocultos);
     actualizar_contenido_derecho();

    //Arrancar el hilo del reloj
     pthread_create(&hilo_reloj, nullptr, actualizar_reloj, nullptr);

    int ch = 0;
    do {
        // Redibujar todos los paneles
        pthread_mutex_lock(&mutex_pantalla);
        dibujar_panel_izquierdo();
        dibujar_panel_derecho();
        dibujar_barra_inferior(usuario);
        pthread_mutex_unlock(&mutex_pantalla);

        // Esperar tecla en el panel izquierdo (el activo)
        ch = wgetch(win_izq);

        // Detectar redimensionamiento de pantalla
        if (ch == KEY_RESIZE) {
            pthread_mutex_lock(&mutex_pantalla);
            cleanup_terminal();
            initscr(); // Re-inicializar stdscr
            getmaxyx(stdscr, TERM_ROWS, TERM_COLS);
            crear_paneles();
            // Recargar contenido para ajustar scrolls
            entradas_actuales = leer_directorio(ruta_actual, mostrar_ocultos);
            actualizar_contenido_derecho();
            pthread_mutex_unlock(&mutex_pantalla);
            continue;
        }

       pthread_mutex_lock(&mutex_pantalla);
        switch (ch) {

            case KEY_UP:
                if (indice_sel > 0) {
                    indice_sel--;
                    if (indice_sel < scroll_offset)
                        scroll_offset--;
                    scroll_der = 0;
                    actualizar_contenido_derecho();
                }
                break;

            case KEY_DOWN:
                if (indice_sel < (int)entradas_actuales.size() - 1) {
                    indice_sel++;
                    int filas_disp = getmaxy(win_izq) - 5;
                    if (indice_sel >= scroll_offset + filas_disp)
                        scroll_offset++;
                    scroll_der = 0;
                    actualizar_contenido_derecho();
                }
                break;

            case KEY_PPAGE: // Page Up para el panel derecho
                if (scroll_der > 0) {
                    scroll_der -= 5;
                    if (scroll_der < 0) scroll_der = 0;
                }
                break;

            case KEY_NPAGE: // Page Down para el panel derecho
                if (scroll_der + (getmaxy(win_der) - 2) < (int)contenido_vista_der.size()) {
                    scroll_der += 5;
                }
                break;

            case KEY_F(2):
                vista_actual = VISTA_TEXTO;
                scroll_der = 0;
                actualizar_contenido_derecho();
                break;

            case KEY_F(3):
                vista_actual = VISTA_HEX;
                scroll_der = 0;
                actualizar_contenido_derecho();
                break;

            case KEY_F(4):
                vista_actual = VISTA_PROPS;
                scroll_der = 0;
                // No necesita actualizar_contenido_derecho ya que lee stat al dibujar
                break;

            case KEY_F(5):
                vista_actual = VISTA_ARBOL;
                scroll_der = 0;
                actualizar_contenido_derecho();
                break;

            case 10:
            case KEY_ENTER:
                if (!entradas_actuales.empty()) {
                    const Entrada &e = entradas_actuales[indice_sel];
                    if (e.es_dir) {
                        if (e.nombre == "..") {
                            // Directorio padre
                            auto pos = ruta_actual.rfind('/');
                            if (pos != std::string::npos && pos != 0)
                                ruta_actual = ruta_actual.substr(0, pos);
                            else if (pos == 0)
                                ruta_actual = "/";
                        } else {
                            if (ruta_actual == "/") ruta_actual = "/" + e.nombre;
                            else ruta_actual += "/" + e.nombre;
                        }
                        entradas_actuales = leer_directorio(ruta_actual, mostrar_ocultos);
                        indice_sel   = 0;
                        scroll_offset = 0;
                        scroll_der = 0;
                        actualizar_contenido_derecho();
                    }
                }
                break;
            case 'n':
            case 'N':
                 if(!entradas_actuales.empty()){
                  const Entrada &e = entradas_actuales[indice_sel];
                  if(!e.es_dir){
                  std::string ruta_completa = ruta_actual + "/" + e.nombre;
                  if(es_texto(ruta_completa)){
                   abrir_en_nano(ruta_completa);
                    //Redibujar todo al regresar del nano
                    clear();
                    refresh();
                    crear_paneles();
                    entradas_actuales = leer_directorio(ruta_actual, mostrar_ocultos);
                    actualizar_contenido_derecho();
                     }else{
                       //Mostrar mensaje si no es texto
                       mvwprintw(win_barra, 2, 2, "No es un archivo de texto ASCII");
                       wrefresh(win_barra);
                       napms(1500);  // Mostrar 1.5 segundos
                      }
                   }
                 }
                break;
            case 'h':
            case 'H':
                mostrar_ocultos = !mostrar_ocultos;
                entradas_actuales = leer_directorio(ruta_actual, mostrar_ocultos);
                indice_sel   = 0;
                scroll_offset = 0;
                scroll_der = 0;
                actualizar_contenido_derecho();
                break;

            case 'i':
            case 'I':
                mostrar_inodos = !mostrar_inodos;
                break;
            case 'q':
            case 'Q':
                // Salir
                break;

        }
      pthread_mutex_unlock(&mutex_pantalla);
    } while (ch != 'q' && ch != 'Q');
    hilo_activo = false;
    pthread_join(hilo_reloj, nullptr);
}

// ============================================================
//  main()
// ============================================================
int main(int argc, char *argv[]) {
    // Directorio inicial: argumento o directorio actual
    std::string ruta_inicial = (argc > 1) ? argv[1] : ".";

    // Limpiar el terminal al salir aunque haya crash
    atexit(cleanup_terminal);

    // 1. Inicializar ncurses
    init_terminal();

    // 3. Crear los tres paneles
    crear_paneles();

    // 4. Obtener usuario actual (POSIX)
    std::string usuario = obtener_usuario();

    // 5. Entrar al loop principal
      ruta_actual=ruta_inicial;
      loop_principal(usuario);
    // 6. cleanup_terminal() se llama automaticamente por atexit()
    return 0;
}
