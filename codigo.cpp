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

using namespace std;

int TERM_ROWS, TERM_COLS;

WINDOW *win_izq   = nullptr;  //Ventana Izquierda
WINDOW *win_der   = nullptr;  //Ventana Derecha
WINDOW *win_barra = nullptr;  //Ventana Inferior

// ── Enumeración para controlar los modos de la vista derecha ──
enum ModoVista { VISTA_TEXTO, VISTA_HEX, VISTA_PROPS, VISTA_ARBOL };
ModoVista vista_actual = VISTA_TEXTO;

// ── Estructura que representa un archivo o directorio ─────────
struct Entrada {
    string nombre;       // Nombre del archivo
    string tamanio;      // "1.2 KiB", "3.4 MiB", etc.
    string permisos;     // "-rwxr-xr-x"
    string fecha;        // "2024-05-01"
    string tipo;         // "Dir", "PDF", "Imagen", etc.
    ino_t       inodo;        // Número de i-nodo
    bool        es_dir;       // true si es directorio
    bool        es_ejecutable;
    off_t       tamanio_raw;  // Tamaño en bytes (para ordenar)
};

vector<Entrada> entradas_actuales;
int    indice_sel      = 0;   // Entrada seleccionada en el panel izquierdo
int    scroll_offset   = 0;   // Para scroll si hay muchos archivos (izquierdo)

// ── Variables para el control del panel derecho ───────────────
vector<string> contenido_vista_der; // Líneas de texto/hex a mostrar
int    scroll_der      = 0;   // Desplazamiento vertical en el panel derecho

bool   mostrar_ocultos = false;
bool   mostrar_inodos  = false;
string ruta_actual = ".";

atomic<bool> hilo_activo(true);
atomic<bool> necesita_refresco(true);
pthread_t hilo_reloj, hilo_refresco;
pthread_mutex_t mutex_pantalla = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  cond_refresco  = PTHREAD_COND_INITIALIZER;

// ── Enumeración para el orden de la lista ─────────────────────
enum ModoOrden { ORD_NOMBRE, ORD_TAMANIO, ORD_FECHA, ORD_TIPO };
ModoOrden orden_actual = ORD_NOMBRE;
bool      orden_asc    = true;

// ── Definición de IDs de color ───────────────────────────────
#define COLOR_SELECCIONADO 1
#define COLOR_DIRECTORIO   2
#define COLOR_EJECUTABLE   3
#define COLOR_NORMAL       4
#define COLOR_BARRA        5
#define COLOR_ESPECIAL     6
#define COLOR_BORDE        7
#define COLOR_TITULO       8

// ── Prototipos de funciones ──────────────────────────────────
void dibujar_panel_izquierdo();
void dibujar_panel_derecho();
void dibujar_barra_inferior(const string &usuario);
string pedir_entrada(const string &prompt);
void ejecutar_comando(const vector<string> &args);

// ── Funciones Auxiliares ─────────────────────────────────────

string formatear_tamanio(off_t bytes) {
    const char *unidades[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    int i = 0;
    double d = bytes;
    while (d >= 1024 && i < 4) {
        d /= 1024;
        i++;
    }
    char buf[32];
    sprintf(buf, "%.1f %s", d, unidades[i]);
    return string(buf);
}

string formatear_permisos(mode_t mode) {
    char buf[11];
    buf[0] = S_ISDIR(mode) ? 'd' : (S_ISLNK(mode) ? 'l' : '-');
    buf[1] = (mode & S_IRUSR) ? 'r' : '-';
    buf[2] = (mode & S_IWUSR) ? 'w' : '-';
    buf[3] = (mode & S_IXUSR) ? 'x' : '-';
    buf[4] = (mode & S_IRGRP) ? 'r' : '-';
    buf[5] = (mode & S_IWGRP) ? 'w' : '-';
    buf[6] = (mode & S_IXGRP) ? 'x' : '-';
    buf[7] = (mode & S_IROTH) ? 'r' : '-';
    buf[8] = (mode & S_IWOTH) ? 'w' : '-';
    buf[9] = (mode & S_IXOTH) ? 'x' : '-';
    buf[10] = '\0';
    return string(buf);
}

string detectar_tipo(const string &ruta, mode_t mode) {
    if (S_ISDIR(mode)) return "Dir";
    if (S_ISLNK(mode)) return "Link";
    
    size_t dot = ruta.find_last_of('.');
    if (dot != string::npos) {
        string ext = ruta.substr(dot + 1);
        for (auto &c : ext) c = tolower(c);
        if (ext == "pdf") return "PDF";
        if (ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "gif") return "Img";
        if (ext == "zip" || ext == "tar" || ext == "gz" || ext == "rar") return "Zip";
        if (ext == "cpp" || ext == "c" || ext == "h" || ext == "hpp") return "Src";
        if (ext == "txt" || ext == "md" || ext == "json") return "Txt";
    }
    
    if (mode & S_IXUSR) return "Exe";
    return "File";
}

string pedir_entrada(const string &prompt) {
    // Nota: El mutex ya debe estar bloqueado por el llamador (loop_principal)
    echo();
    curs_set(1);
    werase(win_barra);
    wbkgd(win_barra, COLOR_PAIR(COLOR_BARRA));
    mvwprintw(win_barra, 1, 2, "%s: ", prompt.c_str());
    wrefresh(win_barra);
    
    char input[256];
    input[0] = '\0';
    wgetnstr(win_barra, input, 255);
    
    noecho();
    curs_set(0);
    return string(input);
}

// ── Une una ruta y un nombre de archivo de forma segura ──────
string unir_ruta(const string &dir, const string &nombre) {
    if (dir == "/") return "/" + nombre;
    if (dir == ".") return nombre;
    return dir + "/" + nombre;
}

// ── ESTRUCTURA PARA MENSAJES IPC ───────────────────────────
struct MensajeIPC {
    int codigo_error;
    char descripcion[256];
};

// ── EJECUTAR COMANDO CON IPC (PIPES) ─────────────────────────
// Cumple Requerimiento 1 (Procesos) e IPC (Requerimiento 3)
void ejecutar_comando(const vector<string> &args) {
    if (args.empty()) return;
    
    int pipe_fd[2];
    if (pipe(pipe_fd) == -1) {
        perror("pipe");
        return;
    }

    pid_t pid = fork();
    if (pid == 0) {
        // --- PROCESO HIJO ---
        close(pipe_fd[0]); // Cerrar lectura en el hijo

        // Redirigir stderr al pipe para capturar errores (IPC)
        dup2(pipe_fd[1], STDERR_FILENO);

        vector<char*> c_args;
        for (const auto &arg : args) {
            c_args.push_back(const_cast<char*>(arg.c_str()));
        }
        c_args.push_back(nullptr);
        
        execvp(c_args[0], c_args.data());
        
        // Si execvp falla, enviamos el error por el pipe antes de salir
        perror("execvp");
        exit(1);
    } else if (pid > 0) {
        // --- PROCESO PADRE ---
        close(pipe_fd[1]); // Cerrar escritura en el padre

        char buffer[512];
        string error_msg = "";
        ssize_t n;
        while ((n = read(pipe_fd[0], buffer, sizeof(buffer) - 1)) > 0) {
            buffer[n] = '\0';
            error_msg += buffer;
        }
        close(pipe_fd[0]);

        int status;
        waitpid(pid, &status, 0);

        if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
            pthread_mutex_lock(&mutex_pantalla);
            def_prog_mode();
            endwin();
            printf("\n--- ERROR EN OPERACION ---\n");
            printf("Codigo de salida: %d\n", WEXITSTATUS(status));
            if (!error_msg.empty()) printf("Detalle: %s\n", error_msg.c_str());
            printf("\nPresione ENTER para volver...");
            getchar();
            reset_prog_mode();
            refresh();
            pthread_mutex_unlock(&mutex_pantalla);
        }
    } else {
        perror("fork");
    }
}

// ── Lee el directorio y retorna vector de Entradas ────────────
vector<Entrada> leer_directorio(const string &ruta, bool mostrar_ocultos) {
    vector<Entrada> entradas;

    DIR *dir = opendir(ruta.c_str());
    if (!dir) return entradas;

    struct dirent *dp;
    while ((dp = readdir(dir)) != nullptr) {
        string nombre = dp->d_name;
        if (nombre == ".") continue;
        if (!mostrar_ocultos && nombre[0] == '.' && nombre != "..") continue;

        string ruta_completa = (ruta == "/" ? "/" : ruta + "/") + nombre;
        struct stat st;
        if (lstat(ruta_completa.c_str(), &st) != 0) continue;

        Entrada e;
        e.nombre        = nombre;
        e.inodo         = st.st_ino;
        e.es_dir        = S_ISDIR(st.st_mode);
        e.es_ejecutable = (st.st_mode & S_IXUSR) && !e.es_dir;
        e.tamanio_raw   = st.st_size;
        e.tamanio       = e.es_dir ? "<DIR>" : formatear_tamanio(st.st_size);
        e.permisos      = formatear_permisos(st.st_mode);
        e.tipo          = detectar_tipo(ruta_completa, st.st_mode);
        
        char buf[20];
        strftime(buf, sizeof(buf), "%Y-%m-%d", localtime(&st.st_mtime));
        e.fecha = buf;

        entradas.push_back(e);
    }
    closedir(dir);

    // ── Ordenamiento Dinámico ────────────────────────────────
    sort(entradas.begin(), entradas.end(), [](const Entrada &a, const Entrada &b) {
        if (a.nombre == "..") return true;
        if (b.nombre == "..") return false;
        if (a.es_dir != b.es_dir) return a.es_dir > b.es_dir;

        bool res = false;
        if (orden_actual == ORD_NOMBRE) res = a.nombre < b.nombre;
        else if (orden_actual == ORD_TAMANIO) res = a.tamanio_raw < b.tamanio_raw;
        else if (orden_actual == ORD_FECHA)   res = a.fecha < b.fecha;
        else if (orden_actual == ORD_TIPO)    res = a.tipo < b.tipo;
        
        return orden_asc ? res : !res;
    });

    return entradas;
}

// ── Hilo de refresco: Dibuja cuando hay cambios ───────────────
void *hilo_refresco_func(void *arg) {
    const string *user = (const string*)arg;
    while (hilo_activo) {
        pthread_mutex_lock(&mutex_pantalla);
        while (!necesita_refresco && hilo_activo) {
            pthread_cond_wait(&cond_refresco, &mutex_pantalla);
        }
        if (!hilo_activo) {
            pthread_mutex_unlock(&mutex_pantalla);
            break;
        }
        
        dibujar_panel_izquierdo();
        dibujar_panel_derecho();
        dibujar_barra_inferior(*user);
        necesita_refresco = false;
        
        pthread_mutex_unlock(&mutex_pantalla);
    }
    return nullptr;
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
void dibujar_borde_titulo(WINDOW *win, const string &titulo) {
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
string obtener_usuario() {
    struct passwd *pw = getpwuid(getuid());
    return pw ? string(pw->pw_name) : "desconocido";
}

// ── Detecta si un archivo es texto ASCII ─────────────────────
bool es_texto(const string &ruta) {
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

void cargar_vista_texto(const string &ruta) {
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
void cargar_vista_hex(const string &ruta) {
    contenido_vista_der.clear();
    FILE *f = fopen(ruta.c_str(), "rb");
    if (!f) return;

    unsigned char buffer[16];
    size_t n;
    unsigned int offset = 0;

    while ((n = fread(buffer, 1, 16, f)) > 0 && contenido_vista_der.size() < 2000) {
        stringstream ss;
        // Offset: 00000000
        ss << hex << setw(8) << setfill('0') << offset << ": ";

        // Hex bytes: 7f 45 4c 46 ...
        for (size_t i = 0; i < 16; i++) {
            if (i < n)
                ss << hex << setw(2) << setfill('0') << (int)buffer[i] << " ";
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
void generar_arbol(const string &ruta, int nivel, string prefijo, bool es_ultimo) {
    if (nivel > 3) return; // Limitar profundidad por rendimiento

    DIR *dir = opendir(ruta.c_str());
    if (!dir) return;

    struct dirent *dp;
    vector<string> sub;
    while ((dp = readdir(dir)) != nullptr) {
        string n = dp->d_name;
        if (n == "." || n == "..") continue;
        if (!mostrar_ocultos && n[0] == '.') continue;
        sub.push_back(n);
    }
    closedir(dir);
    sort(sub.begin(), sub.end());

    for (size_t i = 0; i < sub.size(); i++) {
        bool ultimo_hijo = (i == sub.size() - 1);
        string conector = ultimo_hijo ? "+-- " : "|-- ";
        contenido_vista_der.push_back(prefijo + conector + sub[i]);

        string nueva_ruta = ruta + "/" + sub[i];
        struct stat st;
        if (lstat(nueva_ruta.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            generar_arbol(nueva_ruta, nivel + 1, prefijo + (ultimo_hijo ? "    " : "|   "), ultimo_hijo);
        }
    }
}

// ── Dibuja las propiedades del archivo en win_der ──────────────
void dibujar_propiedades(const string &ruta) {
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
    string ruta_completa = ruta_actual + "/" + e.nombre;

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
    
    string titulo = "Vista";
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
            string linea = contenido_vista_der[i + scroll_der];
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
void dibujar_barra_inferior(const string &usuario) {
    werase(win_barra);
    wbkgd(win_barra, COLOR_PAIR(COLOR_BARRA));

    // Fila 0: Usuario y Atajos básicos
    wattron(win_barra, COLOR_PAIR(COLOR_BARRA) | A_BOLD);
    mvwprintw(win_barra, 0, 1, " %s ", usuario.c_str());
    wattroff(win_barra, A_BOLD);

    mvwprintw(win_barra, 0, 12, "F2:Txt F3:Hex F4:Prop F5:Arb | 1:Nom 2:Tam 3:Fec 4:Tip");
    
    // Fila 1: Operaciones y Reloj
    mvwprintw(win_barra, 1, 1, "C:Copy M:Move B:Del P:Chmod A:New K:Dir E:Exec N:Nano H:Hid I:Ino Q:Exit");

    // Reloj
    time_t ahora = time(nullptr);
    char buf_hora[32];
    strftime(buf_hora, sizeof(buf_hora), "%H:%M:%S", localtime(&ahora));

    wattron(win_barra, COLOR_PAIR(COLOR_BARRA) | A_BOLD);
    int cols = getmaxx(win_barra);
    mvwprintw(win_barra, 1, cols - 10, "%s", buf_hora);
    wattroff(win_barra, COLOR_PAIR(COLOR_BARRA) | A_BOLD);

    wrefresh(win_barra);
}

// ── Abre un archivo en nano como proceso hijo ─────────────────
void abrir_en_nano(const string &ruta_archivo) {
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
            string usuario = obtener_usuario();
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
void loop_principal(const string &usuario){
    // Cargar directorio Inicial
    entradas_actuales = leer_directorio(ruta_actual, mostrar_ocultos);
    actualizar_contenido_derecho();

    // Arrancar hilos
    pthread_create(&hilo_reloj, nullptr, actualizar_reloj, nullptr);
    pthread_create(&hilo_refresco, nullptr, hilo_refresco_func, (void*)&usuario);

    int ch = 0;
    do {
        // Notificar al hilo de refresco que debe redibujar
        pthread_mutex_lock(&mutex_pantalla);
        necesita_refresco = true;
        pthread_cond_signal(&cond_refresco);
        pthread_mutex_unlock(&mutex_pantalla);

        // Esperar tecla en el panel izquierdo
        ch = wgetch(win_izq);

        if (ch == KEY_RESIZE) {
            pthread_mutex_lock(&mutex_pantalla);
            cleanup_terminal();
            init_terminal();
            crear_paneles();
            entradas_actuales = leer_directorio(ruta_actual, mostrar_ocultos);
            actualizar_contenido_derecho();
            necesita_refresco = true;
            pthread_cond_signal(&cond_refresco);
            pthread_mutex_unlock(&mutex_pantalla);
            continue;
        }

        pthread_mutex_lock(&mutex_pantalla);
        switch (ch) {
            case KEY_UP:
                if (indice_sel > 0) {
                    indice_sel--;
                    if (indice_sel < scroll_offset) scroll_offset--;
                    scroll_der = 0;
                    actualizar_contenido_derecho();
                }
                break;
            case KEY_DOWN:
                if (indice_sel < (int)entradas_actuales.size() - 1) {
                    indice_sel++;
                    int filas_disp = getmaxy(win_izq) - 5;
                    if (indice_sel >= scroll_offset + filas_disp) scroll_offset++;
                    scroll_der = 0;
                    actualizar_contenido_derecho();
                }
                break;
            case KEY_F(2): vista_actual = VISTA_TEXTO; scroll_der = 0; actualizar_contenido_derecho(); break;
            case KEY_F(3): vista_actual = VISTA_HEX; scroll_der = 0; actualizar_contenido_derecho(); break;
            case KEY_F(4): vista_actual = VISTA_PROPS; scroll_der = 0; break;
            case KEY_F(5): vista_actual = VISTA_ARBOL; scroll_der = 0; actualizar_contenido_derecho(); break;
            
            // ── Ordenamiento ──────────────────────────────────
            case '1': orden_actual = ORD_NOMBRE; entradas_actuales = leer_directorio(ruta_actual, mostrar_ocultos); break;
            case '2': orden_actual = ORD_TAMANIO; entradas_actuales = leer_directorio(ruta_actual, mostrar_ocultos); break;
            case '3': orden_actual = ORD_FECHA; entradas_actuales = leer_directorio(ruta_actual, mostrar_ocultos); break;
            case '4': orden_actual = ORD_TIPO; entradas_actuales = leer_directorio(ruta_actual, mostrar_ocultos); break;

            case 10:
            case KEY_ENTER:
                if (!entradas_actuales.empty()) {
                    const Entrada &e = entradas_actuales[indice_sel];
                    if (e.es_dir) {
                        if (e.nombre == "..") {
                            auto pos = ruta_actual.rfind('/');
                            if (pos != string::npos && pos != 0) ruta_actual = ruta_actual.substr(0, pos);
                            else if (pos == 0) ruta_actual = "/";
                        } else {
                            if (ruta_actual == "/") ruta_actual = "/" + e.nombre;
                            else ruta_actual += "/" + e.nombre;
                        }
                        entradas_actuales = leer_directorio(ruta_actual, mostrar_ocultos);
                        indice_sel = 0; scroll_offset = 0; scroll_der = 0;
                        actualizar_contenido_derecho();
                    }
                }
                break;

            // ── Operaciones de Archivo ────────────────────────
            case 'c':
            case 'C': {
                if (entradas_actuales.empty()) break;
                string destino = pedir_entrada("Copiar a (ruta/nombre)");
                if (!destino.empty()) {
                    def_prog_mode(); endwin();
                    ejecutar_comando({"cp", "-r", unir_ruta(ruta_actual, entradas_actuales[indice_sel].nombre), destino});
                    reset_prog_mode(); refresh();
                    entradas_actuales = leer_directorio(ruta_actual, mostrar_ocultos);
                }
                break;
            }
            case 'm':
            case 'M': {
                if (entradas_actuales.empty()) break;
                string destino = pedir_entrada("Mover/Renombrar a");
                if (!destino.empty()) {
                    def_prog_mode(); endwin();
                    ejecutar_comando({"mv", unir_ruta(ruta_actual, entradas_actuales[indice_sel].nombre), destino});
                    reset_prog_mode(); refresh();
                    entradas_actuales = leer_directorio(ruta_actual, mostrar_ocultos);
                }
                break;
            }
            case 'b':
            case 'B': {
                if (entradas_actuales.empty() || entradas_actuales[indice_sel].nombre == "..") break;
                string conf = pedir_entrada("¿Borrar " + entradas_actuales[indice_sel].nombre + "? (s/n)");
                if (conf == "s" || conf == "S") {
                    def_prog_mode(); endwin();
                    ejecutar_comando({"rm", "-rf", unir_ruta(ruta_actual, entradas_actuales[indice_sel].nombre)});
                    reset_prog_mode(); refresh();
                    entradas_actuales = leer_directorio(ruta_actual, mostrar_ocultos);
                    if (indice_sel >= (int)entradas_actuales.size()) indice_sel = (int)entradas_actuales.size() - 1;
                    if (indice_sel < 0) indice_sel = 0;
                }
                break;
            }
            case 'p':
            case 'P': {
                if (entradas_actuales.empty()) break;
                string modo = pedir_entrada("Nuevo modo (octal, ej: 755)");
                if (!modo.empty()) {
                    def_prog_mode(); endwin();
                    ejecutar_comando({"chmod", modo, unir_ruta(ruta_actual, entradas_actuales[indice_sel].nombre)});
                    reset_prog_mode(); refresh();
                    entradas_actuales = leer_directorio(ruta_actual, mostrar_ocultos);
                }
                break;
            }
            case 'a':
            case 'A': {
                string nuevo = pedir_entrada("Nombre del nuevo archivo");
                if (!nuevo.empty()) {
                    string r = unir_ruta(ruta_actual, nuevo);
                    def_prog_mode(); endwin();
                    ejecutar_comando({"touch", r});
                    reset_prog_mode(); refresh();
                    // Usar --wait para que el gestor espere a que cierres la ventana
                    ejecutar_comando({"gnome-terminal", "--wait", "--", "nano", r});
                    entradas_actuales = leer_directorio(ruta_actual, mostrar_ocultos);
                    actualizar_contenido_derecho();
                }
                break;
            }
            case 'k':
            case 'K': {
                string nuevo = pedir_entrada("Nombre del nuevo directorio");
                if (!nuevo.empty()) {
                    def_prog_mode(); endwin();
                    ejecutar_comando({"mkdir", "-p", unir_ruta(ruta_actual, nuevo)});
                    reset_prog_mode(); refresh();
                    entradas_actuales = leer_directorio(ruta_actual, mostrar_ocultos);
                }
                break;
            }
            case 'e':
            case 'E': {
                if (entradas_actuales.empty()) break;
                const Entrada &e = entradas_actuales[indice_sel];
                string cmd = unir_ruta(ruta_actual, e.nombre);
                // Lanzar en una nueva ventana de terminal (Requisito de "otra ventana")
                // Se usa sh -c para que la ventana no se cierre inmediatamente al terminar
                ejecutar_comando({"gnome-terminal", "--", "sh", "-c", cmd + "; echo; echo 'Proceso terminado. Presione una tecla para cerrar...'; read -n 1"});
                break;
            }
            case 'n':
            case 'N':
                if(!entradas_actuales.empty()){
                    const Entrada &e = entradas_actuales[indice_sel];
                    if(!e.es_dir){
                        string r = unir_ruta(ruta_actual, e.nombre);
                        if(es_texto(r)) {
                            // Abrir nano en una nueva ventana y esperar a que cierre
                            ejecutar_comando({"gnome-terminal", "--wait", "--", "nano", r});
                            entradas_actuales = leer_directorio(ruta_actual, mostrar_ocultos);
                            actualizar_contenido_derecho();
                        }
                    }
                }
                break;
            case 'h':
            case 'H':
                mostrar_ocultos = !mostrar_ocultos;
                entradas_actuales = leer_directorio(ruta_actual, mostrar_ocultos);
                indice_sel = 0; scroll_offset = 0; scroll_der = 0;
                actualizar_contenido_derecho();
                break;
            case 'i':
            case 'I': mostrar_inodos = !mostrar_inodos; break;
        }
        necesita_refresco = true;
        pthread_cond_signal(&cond_refresco);
        pthread_mutex_unlock(&mutex_pantalla);
    } while (ch != 'q' && ch != 'Q');

    hilo_activo = false;
    pthread_cond_signal(&cond_refresco);
    pthread_join(hilo_reloj, nullptr);
    pthread_join(hilo_refresco, nullptr);
}

// ============================================================
//  main()
// ============================================================
int main(int argc, char *argv[]) {
    // Directorio inicial: argumento o directorio actual
    string ruta_inicial = (argc > 1) ? argv[1] : ".";

    // Limpiar el terminal al salir aunque haya crash
    atexit(cleanup_terminal);

    // 1. Inicializar ncurses
    init_terminal();

    // 3. Crear los tres paneles
    crear_paneles();

    // 4. Obtener usuario actual (POSIX)
    string usuario = obtener_usuario();

    // 5. Entrar al loop principal
      ruta_actual=ruta_inicial;
      loop_principal(usuario);
    // 6. cleanup_terminal() se llama automaticamente por atexit()
    return 0;
}
