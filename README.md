# Gestor de Archivos para Consola en Linux

## Descripcion
Este proyecto es un gestor de archivos interactivo desarrollado en C++ utilizando la biblioteca ncurses. Permite la navegacion por el sistema de archivos de Linux a traves de una interfaz de doble panel, proporcionando funcionalidades para la gestion de archivos, previsualizacion de contenido y ejecucion de programas en terminales independientes.

## Requisitos del Sistema
Para compilar y ejecutar este proyecto, es necesario tener instalados los siguientes paquetes en un sistema basado en Linux:
- Compilador GCC (g++)
- Herramienta Make
- Biblioteca de desarrollo ncurses (libncurses5-dev o similar)
- Emulador de terminal gnome-terminal

## Funcionalidades Principales
- Navegacion por directorios (flechas arriba/abajo y Enter).
- Copia de archivos y directorios (C).
- Movimiento y renombrado de archivos (M).
- Eliminacion de archivos con confirmacion (B).
- Creacion de nuevos archivos (A) y directorios (K).
- Modificacion de permisos de archivos (P).
- Visualizacion de contenido en formato texto (F2), hexadecimal (F3), propiedades (F4) y arbol (F5).
- Ejecucion de binarios en una nueva ventana de terminal (E).
- Edicion de archivos con Nano en una nueva ventana (N).

## Instalacion de Dependencias
Para instalar las bibliotecas necesarias en sistemas basados en Debian/Ubuntu, ejecute:
sudo apt update && sudo apt install libncurses5-dev libncursesw5-dev make g++

## Pasos para la Ejecucion

1. Acceda al directorio del proyecto:
   cd "Proyecto de SO"

2. Limpie los archivos de compilacion previos (opcional):
   make clean

3. Compile y ejecute el programa:
   make run

4. Para salir del gestor de archivos, presione la tecla 'Q'.
