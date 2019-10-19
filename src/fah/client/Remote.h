/******************************************************************************\

                  This file is part of the Folding@home Client.

           The fahclient runs Folding@home protein folding simulations.
                    Copyright (c) 2001-2019, foldingathome.org
                               All rights reserved.

       This program is free software; you can redistribute it and/or modify
       it under the terms of the GNU General Public License as published by
        the Free Software Foundation; either version 2 of the License, or
                       (at your option) any later version.

         This program is distributed in the hope that it will be useful,
          but WITHOUT ANY WARRANTY; without even the implied warranty of
          MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
                   GNU General Public License for more details.

     You should have received a copy of the GNU General Public License along
     with this program; if not, write to the Free Software Foundation, Inc.,
           51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

                  For information regarding this software email:
                                 Joseph Coffland
                          joseph@cauldrondevelopment.com

\******************************************************************************/

#pragma once

#include <cbang/event/JSONWebsocket.h>


namespace FAH {
  namespace Client {
    class App;


    class Remote : public cb::Event::JSONWebsocket {
      App &app;

    public:
      Remote(App &app, cb::Event::RequestMethod method, const cb::URI &uri,
             const cb::Version &version);

      // From cb::Event::JSONWebsocket
      void onMessage(const cb::JSON::ValuePtr &msg);

      // From cb::Event::Websocket
      void onOpen();

      // From cb::Event::Request
      void onComplete();
    };
  }
}
