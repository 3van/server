/*
* Copyright (c) 2011 Sveriges Television AB <info@casparcg.com>
*
* This file is part of CasparCG (www.casparcg.com).
*
* CasparCG is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* CasparCG is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with CasparCG. If not, see <http://www.gnu.org/licenses/>.
*
* Author: Robert Nagy, ronag89@gmail.com
*/

#include "../../stdafx.h"

#include "video_decoder.h"

#include "../util/util.h"

#include "../../ffmpeg_error.h"

#include <common/log.h>
#include <core/frame/frame_transform.h>
#include <core/frame/frame_factory.h>

#include <boost/range/algorithm_ext/push_back.hpp>
#include <boost/filesystem.hpp>

#include <queue>

#if defined(_MSC_VER)
#pragma warning (push)
#pragma warning (disable : 4244)
#endif
extern "C" 
{
	#include <libavcodec/avcodec.h>
	#include <libavformat/avformat.h>
}
#if defined(_MSC_VER)
#pragma warning (pop)
#endif

namespace caspar { namespace ffmpeg {
	
struct video_decoder::impl : boost::noncopyable
{
	monitor::basic_subject					event_subject_;
	int										index_;
	const std::shared_ptr<AVCodecContext>	codec_context_;

	std::queue<spl::shared_ptr<AVPacket>>	packets_;
	
	const uint32_t							nb_frames_;

	const int								width_;
	const int								height_;
	bool									is_progressive_;

	uint32_t								file_frame_number_;

public:
	explicit impl() 
		: index_(0)
		, nb_frames_(0)
		, width_(0)
		, height_(0)
		, is_progressive_(true)
		, file_frame_number_(0)
	{
	}

	explicit impl(const spl::shared_ptr<AVFormatContext>& context) 
		: codec_context_(open_codec(*context, AVMEDIA_TYPE_VIDEO, index_))
		, nb_frames_(static_cast<uint32_t>(context->streams[index_]->nb_frames))
		, width_(codec_context_->width)
		, height_(codec_context_->height)
	{
		file_frame_number_ = 0;
	}

	void push(const std::shared_ptr<AVPacket>& packet)
	{
		if(!packet)
			return;

		if(packet->stream_index == index_ || packet->data == nullptr)
			packets_.push(spl::make_shared_ptr(packet));
	}

	std::shared_ptr<AVFrame> poll()
	{		
		if(packets_.empty())
			return nullptr;
		
		auto packet = packets_.front();
		
		if(!codec_context_)		
		{
			packets_.pop();
			return packet->data == nullptr ? flush_video() : empty_video();
		}
		else
		{
			if(packet->data == nullptr)
			{			
				if(codec_context_->codec->capabilities & CODEC_CAP_DELAY)
				{
					auto video = decode(*packet);
					if(video)
						return video;
				}
					
				packets_.pop();
				avcodec_flush_buffers(codec_context_.get());				
				return flush_video();	
			}
			
			packets_.pop();
			return decode(*packet);
		}
		
	}

	std::shared_ptr<AVFrame> decode(AVPacket& pkt)
	{
		std::shared_ptr<AVFrame> decoded_frame(avcodec_alloc_frame(), av_free);

		int frame_finished = 0;
		THROW_ON_ERROR2(avcodec_decode_video2(codec_context_.get(), decoded_frame.get(), &frame_finished, &pkt), "[video_decocer]");
		
		// If a decoder consumes less then the whole packet then something is wrong
		// that might be just harmless padding at the end, or a problem with the
		// AVParser or demuxer which puted more then one frame in a AVPacket.

		if(frame_finished == 0)	
			return nullptr;

		is_progressive_ = !decoded_frame->interlaced_frame;

		if(decoded_frame->repeat_pict > 0)
			CASPAR_LOG(warning) << "[video_decoder] Field repeat_pict not implemented.";
				
		event_subject_  << monitor::event("file/video/width")	% width_
						<< monitor::event("file/video/height")	% height_
						<< monitor::event("file/video/field")	% u8(!decoded_frame->interlaced_frame ? "progressive" : (decoded_frame->top_field_first ? "upper" : "lower"))
						<< monitor::event("file/video/codec")	% u8(codec_context_->codec->long_name);
		
		file_frame_number_ = static_cast<uint32_t>(pkt.pts);

		decoded_frame->pts = file_frame_number_;

		return decoded_frame;
	}
	
	bool ready() const
	{
		return !packets_.empty();
	}

	void clear()
	{
		while(!packets_.empty())
			packets_.pop();
	}

	uint32_t nb_frames() const
	{
		return std::max<uint32_t>(nb_frames_, file_frame_number_);
	}

	std::wstring print() const
	{		
		return L"[video-decoder] " + u16(codec_context_->codec->long_name);
	}
};

video_decoder::video_decoder() : impl_(new impl()){}
video_decoder::video_decoder(const spl::shared_ptr<AVFormatContext>& context) : impl_(new impl(context)){}
video_decoder::video_decoder(video_decoder&& other) : impl_(std::move(other.impl_)){}
video_decoder& video_decoder::operator=(video_decoder&& other){impl_ = std::move(other.impl_); return *this;}
void video_decoder::push(const std::shared_ptr<AVPacket>& packet){impl_->push(packet);}
std::shared_ptr<AVFrame> video_decoder::poll(){return impl_->poll();}
bool video_decoder::ready() const{return impl_->ready();}
int video_decoder::width() const{return impl_->width_;}
int video_decoder::height() const{return impl_->height_;}
uint32_t video_decoder::nb_frames() const{return impl_->nb_frames();}
uint32_t video_decoder::file_frame_number() const{return impl_->file_frame_number_;}
bool	video_decoder::is_progressive() const{return impl_->is_progressive_;}
std::wstring video_decoder::print() const{return impl_->print();}
void video_decoder::clear(){impl_->clear();}
void video_decoder::subscribe(const monitor::observable::observer_ptr& o){impl_->event_subject_.subscribe(o);}
void video_decoder::unsubscribe(const monitor::observable::observer_ptr& o){impl_->event_subject_.unsubscribe(o);}

}}