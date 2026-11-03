<!-- Guía para jugadores y organizadores: torneos dentro del juego (lobby 1.7, servidor de Oracle). -->
# Torneos dentro del juego

Los torneos se juegan **dentro del juego, sin abrir F1**. Te inscribes, confirmas que estás (check-in), el juego te avisa
cuando te toca, tachas escenarios con el control y la pelea empieza sola. Los resultados avanzan solos en el bracket
(doble eliminación: winners y losers). Sirve para torneos chicos y grandes: los sets se juegan todos al mismo tiempo, cada
uno en su propia pelea, sin códigos de sala.

Todo vive en **el servidor del grupo** (el mismo de las Battle Arenas, en Oracle). El organizador maneja el torneo desde
una **página web**.

---

## 1. Conectarte al servidor de torneos (una sola vez)

1. Abre el juego con **Play SSBU.bat** y presiona **F1** (o **Home** en el control).
2. Ve a **Experimental** y pon **Tournaments** en **ON** (está justo debajo de Battle Arenas).
3. Presiona **Restart the game now**. El juego se vuelve a abrir conectado al servidor de torneos.

Todo se juega a través del servidor del grupo (Oracle): no hace falta ninguna otra red ni programa.

Tu **cuenta** se crea sola la primera vez que te conectas. Tu nombre en el torneo es el de **F1 > Experimental > Your
name**. El **código de cuenta** (F1 > Online > Account code) es tu cuenta completa: guárdalo como una contraseña y no lo
compartas.

## 2. La pantalla del torneo

Presiona **F3**, o **mantén presionado − (Menos) un segundo**. Con el control:

| Botón | Qué hace |
|---|---|
| **A** (o Enter) | confirmar: inscribirte, hacer check-in, tachar / banear / elegir el escenario marcado |
| **Cruz** (o flechas) | moverte entre escenarios o torneos |
| **B** (o Esc) | cerrar la pantalla |
| **Y** (o Tab) | ver tu camino en el bracket y las posiciones |
| **X** | salirte del torneo (pide confirmar con A) |

La pantalla **se abre sola** cuando tienes que hacer algo: el check-in, un set nuevo y tu turno de escenario. Nunca se abre
en medio de una pelea. Mientras está abierta, el juego no recibe tus botones.

Arriba a la derecha hay una línea pequeña que dice siempre qué te toca ("YOUR TURN: strike a stage", "Next set: vs …").

## 3. Jugar un torneo

1. **Inscríbete.** Cuando el organizador abre un torneo, sale un aviso arriba ("Tournament open: …"). Presiona **F3**,
   elige el torneo y presiona **A**.
2. **Check-in.** Un rato antes de empezar, la pantalla se abre sola: presiona **A** para confirmar que estás. Quien no
   hace check-in queda fuera cuando se arma el bracket.
3. **Tu set.** Cuando está listo, la pantalla se abre sola con tu rival, la ronda y el marcador.
   - **Game 1:** los dos tachan escenarios (uno tacha 1, el otro 2, el primero 1). El que queda es el escenario.
   - **Game 2 en adelante:** el **ganador** del game anterior banea escenarios y el **perdedor** elige.
   - Cuando es tu turno, mueve la cruz al escenario y presiona **A**. Tienes **60 segundos**: si no eliges, se elige el
     primer escenario que queda.
4. **La pelea.** Cuando el escenario queda elegido, la pantalla dice **"Now: … go to Smash, pick your fighter and press
   start"** y se cierra sola. Ve a **Smash**, elige tu personaje y presiona **start**. El juego espera ahí y **la pelea
   empieza sola** con el escenario elegido, las stocks del torneo, sin objetos y sin peligros del escenario.
   - Desde el game 2, **el ganador elige personaje primero**. Si perdiste el game anterior, ves qué eligió tu rival y
     tienes **10 segundos** para regresar (mantén **B**) y cambiar el tuyo.
5. **Después de cada game** el resultado cuenta solo. En la selección de personaje presiona **start** otra vez (si toca,
   antes banea o elige escenario: la pantalla se abre sola).
6. **Entre sets** deja el juego abierto. La línea de arriba dice contra quién sigue ("Next set: vs winner of A / B").
   Cuando tu set está listo, la pantalla se abre sola.

**Si algo sale mal**
- *"The last game was not counted: play it again"*: los dos juegos no coincidieron (empate, desconexión, alguien salió o
  desincronización). Jueguen ese game otra vez: presionen start de nuevo.
- *Tu rival no aparece:* si tu rival no llega a la selección de personaje en el tiempo que puso el organizador (10
  minutos si no lo cambió; la página del torneo lo dice arriba) y tú sí, **ganas el set** solo. El organizador puede
  deshacerlo.
- *Te quedaste esperando y quieres salir de la espera:* mantén **B** o presiona **Esc** (con la pantalla del torneo
  cerrada).

## 4. Set en stream (opcional)

El organizador puede poner un set **en stream**: ese set se juega en una **Battle Arena** para que los demás lo vean como
espectadores.

1. El organizador (o quien transmite) crea una Battle Arena en el juego (**F1 > Experimental > Battle Arenas** en ON,
   luego Online > Battle Arena) y anota su **código de 5 letras**.
2. En la página web, en ese set, presiona **En stream** y escribe el código.
3. A los dos jugadores la pantalla del torneo les dice **"This set is ON STREAM: play it in the Battle Arena with the
   code …"**. Entran a la arena con ese código y pelean ahí. El resultado cuenta solo para el set.

## 5. Organizar un torneo (en la página web)

Abre **http://160.34.219.118:8080/tournaments** (la dirección del servidor del grupo).

1. **Entrar:** en el juego copia tu código de cuenta (F1 > Online > Account code > Copy) y pégalo en **Entrar como
   organizador**. El código no viaja por internet: el navegador solo firma una prueba con él.
2. **Crear un torneo** (ten el juego abierto y conectado al servidor; el torneo se crea para tu versión del juego): nombre,
   mejor de 1 / 3 / 5 / 7 (sets, top 8 y finales), bans, stocks, check-in, si cuenta para el ladder, minutos para "no se
   presentó" (0: nunca, lo decides tú) y segundos para tachar. Desde el juego (F1 > Tournament > Make a tournament)
   también se eligen los minutos de "no se presentó" (**No-show**).
3. **Inscripciones:** los jugadores se inscriben solos desde el juego. En la lista de jugadores puedes mover seeds
   (flechas ↑ ↓), cambiar el seeding (por rating, al azar, manual) y quitar a alguien.
4. **Check-in:** unos 30 minutos antes presiona **Abrir check-in**. La lista muestra quién confirmó ("listo") y quién
   falta; puedes **Marcar listo** a alguien a mano.
5. **Empezar el torneo:** se arma el bracket solo con los que confirmaron.
6. **Durante el torneo** (la página se actualiza cada 5 segundos), en cada set del bracket puedes:
   - marcar quién ganó (**Ganó …**) si hubo un problema, o **Jugar de nuevo** para borrar el resultado;
   - ponerlo **En stream** (sección 4);
   - cambiar los minutos de **"no se presentó"** (abajo en el panel Organizar, **Guardar**): cuenta también para los sets
     que ya están esperando;
   - en la lista de jugadores: **DQ** a quien no se presentó (pierde el resto de sus sets).
   Lo que sigue en el bracket se acomoda solo.
7. **Cancelar el torneo** lo cancela para todos.

Desde el juego también se puede organizar (F1 > Tournament > Make a tournament), pero con muchos jugadores la página web es
mucho más cómoda.

## 6. El servidor (solo quien lo administra)

El servidor de Oracle ya corre las Battle Arenas. Para encender cuentas y torneos, copia a la VM los archivos nuevos de
`tools/net/` (`ssbu_lobby.py`, `ladder.py`, `tournament.py`, `deploy_lobby.sh`, `deploy_arenas.sh`, `nex_server.py`) y
corre, en esa carpeta:

```
sudo bash deploy_arenas.sh
```

Ahora instala el lobby con cuentas y torneos (`LADDER=on`, `LADDER_OPEN=1`) y publica la página web en el puerto **8080**.
En Oracle Cloud abre también **TCP 8080** (o corre `oci_arenas_ports.sh` en Cloud Shell, que ya lo incluye).

- Solo algunas cuentas pueden crear torneos: `sudo TOURNAMENT_ADMINS="Rober,napky" bash deploy_arenas.sh`.
- Cuentas: `sudo ssbu-account list`, `rename <viejo> <nuevo>`, `ban <nombre>`, `unban <nombre>`, `delete <nombre>`.
- Logs: `journalctl -u ssbu-lobby -f`.
- Iconos de personajes de la página: archivos PNG en `/var/lib/ssbu-lobby/icons/`, con el nombre interno del personaje
  (`mario.png`, `captain.png`, ...: la lista está en `tools/net/tournament.py`, FIGHTERS). No van en el repositorio; si
  falta uno, la página pone las iniciales del personaje.

**Si el servidor se apaga** las cuentas y los torneos se guardan y siguen donde iban cuando vuelve.

## 7. Ver los resultados

En la página del torneo (**http://160.34.219.118:8080/tournaments**, cualquiera la puede abrir, sin entrar):

- Las rondas se llaman como en los torneos de siempre: **Winners Round 1**, **Winners Quarterfinal**, **Winners
  Semifinal**, **Winners Final**, lo mismo en **Losers**, y **Grand Final** / **Grand Final Reset**. El juego usa los
  mismos nombres.
- Al lado de cada jugador salen los **personajes que usó** (en el set, y en Posiciones y Jugadores los de todo el torneo).
- Un set ya jugado dice abajo **"ver escenarios y personajes"**: dale clic y se abre una ventana con cada game, el
  escenario y el personaje de cada uno (el ganador en verde). Se cierra con la ×, con Esc o dando clic afuera.
