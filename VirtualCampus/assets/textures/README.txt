Optional texture overrides.
The app generates all textures itself. To use your own photos instead, put PNG files here
with these exact names (square images, e.g. 512x512, work best):

  facade.png   ONE window of a building wall, floor to floor (it is repeated across every wall).
               Alpha channel: 0 = plaster (gets tinted with the building colour),
               ~0.6 = frame, 1 = glass (shiny, lights up at night). No alpha = all treated as glass.
  grass.png    lawn / ground, repeats every 7 m
  asphalt.png  road surface (only its brightness is used), repeats every 4 m
  paving.png   footpath tiles (brightness only), repeats every 2 m
  roof.png     concrete roof (brightness only), repeats every 5 m
